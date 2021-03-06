/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "shrpx_ssl.h"

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif // HAVE_SYS_SOCKET_H
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif // HAVE_NETDB_H
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/types.h>

#include <vector>
#include <string>
#include <iomanip>

#include <openssl/crypto.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/rand.h>
#include <openssl/dh.h>

#include <nghttp2/nghttp2.h>

#ifdef HAVE_SPDYLAY
#include <spdylay/spdylay.h>
#endif // HAVE_SPDYLAY

#include "shrpx_log.h"
#include "shrpx_client_handler.h"
#include "shrpx_config.h"
#include "shrpx_worker.h"
#include "shrpx_downstream_connection_pool.h"
#include "shrpx_http2_session.h"
#include "shrpx_memcached_request.h"
#include "shrpx_memcached_dispatcher.h"
#include "util.h"
#include "ssl.h"
#include "template.h"

using namespace nghttp2;

namespace shrpx {

namespace ssl {

namespace {
int next_proto_cb(SSL *s, const unsigned char **data, unsigned int *len,
                  void *arg) {
  auto &prefs = get_config()->tls.alpn_prefs;
  *data = prefs.data();
  *len = prefs.size();
  return SSL_TLSEXT_ERR_OK;
}
} // namespace

namespace {
int verify_callback(int preverify_ok, X509_STORE_CTX *ctx) {
  if (!preverify_ok) {
    int err = X509_STORE_CTX_get_error(ctx);
    int depth = X509_STORE_CTX_get_error_depth(ctx);
    LOG(ERROR) << "client certificate verify error:num=" << err << ":"
               << X509_verify_cert_error_string(err) << ":depth=" << depth;
  }
  return preverify_ok;
}
} // namespace

// This function is meant be called from master process, hence the
// call exit(3).
std::vector<unsigned char>
set_alpn_prefs(const std::vector<std::string> &protos) {
  size_t len = 0;

  for (const auto &proto : protos) {
    if (proto.size() > 255) {
      LOG(FATAL) << "Too long ALPN identifier: " << proto.size();
      exit(EXIT_FAILURE);
    }

    len += 1 + proto.size();
  }

  if (len > (1 << 16) - 1) {
    LOG(FATAL) << "Too long ALPN identifier list: " << len;
    exit(EXIT_FAILURE);
  }

  auto out = std::vector<unsigned char>(len);
  auto ptr = out.data();

  for (const auto &proto : protos) {
    *ptr++ = proto.size();
    memcpy(ptr, proto.c_str(), proto.size());
    ptr += proto.size();
  }

  return out;
}

namespace {
int ssl_pem_passwd_cb(char *buf, int size, int rwflag, void *user_data) {
  auto config = static_cast<Config *>(user_data);
  auto len = static_cast<int>(config->tls.private_key_passwd.size());
  if (size < len + 1) {
    LOG(ERROR) << "ssl_pem_passwd_cb: buf is too small " << size;
    return 0;
  }
  // Copy string including last '\0'.
  memcpy(buf, config->tls.private_key_passwd.c_str(), len + 1);
  return len;
}
} // namespace

namespace {
int servername_callback(SSL *ssl, int *al, void *arg) {
  auto conn = static_cast<Connection *>(SSL_get_app_data(ssl));
  auto handler = static_cast<ClientHandler *>(conn->data);
  auto worker = handler->get_worker();
  auto cert_tree = worker->get_cert_lookup_tree();
  if (cert_tree) {
    const char *hostname = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (hostname) {
      auto len = strlen(hostname);
      auto ssl_ctx = cert_tree->lookup(StringRef{hostname, len});
      if (ssl_ctx) {
        SSL_set_SSL_CTX(ssl, ssl_ctx);
      }
    }
  }
  return SSL_TLSEXT_ERR_OK;
}
} // namespace

#ifndef OPENSSL_IS_BORINGSSL
namespace {
std::shared_ptr<std::vector<uint8_t>>
get_ocsp_data(TLSContextData *tls_ctx_data) {
  std::lock_guard<std::mutex> g(tls_ctx_data->mu);
  return tls_ctx_data->ocsp_data;
}
} // namespace

namespace {
int ocsp_resp_cb(SSL *ssl, void *arg) {
  auto ssl_ctx = SSL_get_SSL_CTX(ssl);
  auto tls_ctx_data =
      static_cast<TLSContextData *>(SSL_CTX_get_app_data(ssl_ctx));

  auto data = get_ocsp_data(tls_ctx_data);

  if (!data) {
    return SSL_TLSEXT_ERR_OK;
  }

  auto buf =
      static_cast<uint8_t *>(CRYPTO_malloc(data->size(), __FILE__, __LINE__));

  if (!buf) {
    return SSL_TLSEXT_ERR_OK;
  }

  std::copy(std::begin(*data), std::end(*data), buf);

  SSL_set_tlsext_status_ocsp_resp(ssl, buf, data->size());

  return SSL_TLSEXT_ERR_OK;
}
} // namespace
#endif // OPENSSL_IS_BORINGSSL

constexpr auto MEMCACHED_SESSION_CACHE_KEY_PREFIX =
    StringRef::from_lit("nghttpx:tls-session-cache:");

namespace {
int tls_session_new_cb(SSL *ssl, SSL_SESSION *session) {
  auto conn = static_cast<Connection *>(SSL_get_app_data(ssl));
  auto handler = static_cast<ClientHandler *>(conn->data);
  auto worker = handler->get_worker();
  auto dispatcher = worker->get_session_cache_memcached_dispatcher();

  const unsigned char *id;
  unsigned int idlen;

  id = SSL_SESSION_get_id(session, &idlen);

  if (LOG_ENABLED(INFO)) {
    LOG(INFO) << "Memached: cache session, id=" << util::format_hex(id, idlen);
  }

  auto req = make_unique<MemcachedRequest>();
  req->op = MEMCACHED_OP_ADD;
  req->key = MEMCACHED_SESSION_CACHE_KEY_PREFIX.str();
  req->key += util::format_hex(id, idlen);

  auto sessionlen = i2d_SSL_SESSION(session, nullptr);
  req->value.resize(sessionlen);
  auto buf = &req->value[0];
  i2d_SSL_SESSION(session, &buf);
  req->expiry = 12_h;
  req->cb = [](MemcachedRequest *req, MemcachedResult res) {
    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "Memcached: session cache done.  key=" << req->key
                << ", status_code=" << res.status_code << ", value="
                << std::string(std::begin(res.value), std::end(res.value));
    }
    if (res.status_code != 0) {
      LOG(WARN) << "Memcached: failed to cache session key=" << req->key
                << ", status_code=" << res.status_code << ", value="
                << std::string(std::begin(res.value), std::end(res.value));
    }
  };
  assert(!req->canceled);

  dispatcher->add_request(std::move(req));

  return 0;
}
} // namespace

namespace {
SSL_SESSION *tls_session_get_cb(SSL *ssl, unsigned char *id, int idlen,
                                int *copy) {
  auto conn = static_cast<Connection *>(SSL_get_app_data(ssl));
  auto handler = static_cast<ClientHandler *>(conn->data);
  auto worker = handler->get_worker();
  auto dispatcher = worker->get_session_cache_memcached_dispatcher();

  if (conn->tls.cached_session) {
    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "Memcached: found cached session, id="
                << util::format_hex(id, idlen);
    }

    // This is required, without this, memory leak occurs.
    *copy = 0;

    auto session = conn->tls.cached_session;
    conn->tls.cached_session = nullptr;
    return session;
  }

  if (LOG_ENABLED(INFO)) {
    LOG(INFO) << "Memcached: get cached session, id="
              << util::format_hex(id, idlen);
  }

  auto req = make_unique<MemcachedRequest>();
  req->op = MEMCACHED_OP_GET;
  req->key = MEMCACHED_SESSION_CACHE_KEY_PREFIX.str();
  req->key += util::format_hex(id, idlen);
  req->cb = [conn](MemcachedRequest *, MemcachedResult res) {
    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "Memcached: returned status code " << res.status_code;
    }

    // We might stop reading, so start it again
    conn->rlimit.startw();
    ev_timer_again(conn->loop, &conn->rt);

    conn->wlimit.startw();
    ev_timer_again(conn->loop, &conn->wt);

    conn->tls.cached_session_lookup_req = nullptr;
    if (res.status_code != 0) {
      conn->tls.handshake_state = TLS_CONN_CANCEL_SESSION_CACHE;
      return;
    }

    const uint8_t *p = res.value.data();

    auto session = d2i_SSL_SESSION(nullptr, &p, res.value.size());
    if (!session) {
      if (LOG_ENABLED(INFO)) {
        LOG(INFO) << "cannot materialize session";
      }
      conn->tls.handshake_state = TLS_CONN_CANCEL_SESSION_CACHE;
      return;
    }

    conn->tls.cached_session = session;
    conn->tls.handshake_state = TLS_CONN_GOT_SESSION_CACHE;
  };

  conn->tls.handshake_state = TLS_CONN_WAIT_FOR_SESSION_CACHE;
  conn->tls.cached_session_lookup_req = req.get();

  dispatcher->add_request(std::move(req));

  return nullptr;
}
} // namespace

namespace {
int ticket_key_cb(SSL *ssl, unsigned char *key_name, unsigned char *iv,
                  EVP_CIPHER_CTX *ctx, HMAC_CTX *hctx, int enc) {
  auto conn = static_cast<Connection *>(SSL_get_app_data(ssl));
  auto handler = static_cast<ClientHandler *>(conn->data);
  auto worker = handler->get_worker();
  auto ticket_keys = worker->get_ticket_keys();

  if (!ticket_keys) {
    // No ticket keys available.
    return -1;
  }

  auto &keys = ticket_keys->keys;
  assert(!keys.empty());

  if (enc) {
    if (RAND_bytes(iv, EVP_MAX_IV_LENGTH) == 0) {
      if (LOG_ENABLED(INFO)) {
        CLOG(INFO, handler) << "session ticket key: RAND_bytes failed";
      }
      return -1;
    }

    auto &key = keys[0];

    if (LOG_ENABLED(INFO)) {
      CLOG(INFO, handler) << "encrypt session ticket key: "
                          << util::format_hex(key.data.name);
    }

    std::copy(std::begin(key.data.name), std::end(key.data.name), key_name);

    EVP_EncryptInit_ex(ctx, get_config()->tls.ticket.cipher, nullptr,
                       key.data.enc_key.data(), iv);
    HMAC_Init_ex(hctx, key.data.hmac_key.data(), key.hmac_keylen, key.hmac,
                 nullptr);
    return 1;
  }

  size_t i;
  for (i = 0; i < keys.size(); ++i) {
    auto &key = keys[i];
    if (std::equal(std::begin(key.data.name), std::end(key.data.name),
                   key_name)) {
      break;
    }
  }

  if (i == keys.size()) {
    if (LOG_ENABLED(INFO)) {
      CLOG(INFO, handler) << "session ticket key "
                          << util::format_hex(key_name, 16) << " not found";
    }
    return 0;
  }

  if (LOG_ENABLED(INFO)) {
    CLOG(INFO, handler) << "decrypt session ticket key: "
                        << util::format_hex(key_name, 16);
  }

  auto &key = keys[i];
  HMAC_Init_ex(hctx, key.data.hmac_key.data(), key.hmac_keylen, key.hmac,
               nullptr);
  EVP_DecryptInit_ex(ctx, key.cipher, nullptr, key.data.enc_key.data(), iv);

  return i == 0 ? 1 : 2;
}
} // namespace

namespace {
void info_callback(const SSL *ssl, int where, int ret) {
  // To mitigate possible DOS attack using lots of renegotiations, we
  // disable renegotiation. Since OpenSSL does not provide an easy way
  // to disable it, we check that renegotiation is started in this
  // callback.
  if (where & SSL_CB_HANDSHAKE_START) {
    auto conn = static_cast<Connection *>(SSL_get_app_data(ssl));
    if (conn && conn->tls.initial_handshake_done) {
      auto handler = static_cast<ClientHandler *>(conn->data);
      if (LOG_ENABLED(INFO)) {
        CLOG(INFO, handler) << "TLS renegotiation started";
      }
      handler->start_immediate_shutdown();
    }
  }
}
} // namespace

#if OPENSSL_VERSION_NUMBER >= 0x10002000L
namespace {
int alpn_select_proto_cb(SSL *ssl, const unsigned char **out,
                         unsigned char *outlen, const unsigned char *in,
                         unsigned int inlen, void *arg) {
  // We assume that get_config()->npn_list contains ALPN protocol
  // identifier sorted by preference order.  So we just break when we
  // found the first overlap.
  for (const auto &target_proto_id : get_config()->tls.npn_list) {
    for (auto p = in, end = in + inlen; p < end;) {
      auto proto_id = p + 1;
      auto proto_len = *p;

      if (proto_id + proto_len <= end &&
          util::streq(StringRef{target_proto_id},
                      StringRef{proto_id, proto_len})) {

        *out = reinterpret_cast<const unsigned char *>(proto_id);
        *outlen = proto_len;

        return SSL_TLSEXT_ERR_OK;
      }

      p += 1 + proto_len;
    }
  }

  return SSL_TLSEXT_ERR_NOACK;
}
} // namespace
#endif // OPENSSL_VERSION_NUMBER >= 0x10002000L

struct TLSProtocol {
  StringRef name;
  long int mask;
};

constexpr TLSProtocol TLS_PROTOS[] = {
    TLSProtocol{StringRef::from_lit("TLSv1.2"), SSL_OP_NO_TLSv1_2},
    TLSProtocol{StringRef::from_lit("TLSv1.1"), SSL_OP_NO_TLSv1_1},
    TLSProtocol{StringRef::from_lit("TLSv1.0"), SSL_OP_NO_TLSv1}};

long int create_tls_proto_mask(const std::vector<std::string> &tls_proto_list) {
  long int res = 0;

  for (auto &supported : TLS_PROTOS) {
    auto ok = false;
    for (auto &name : tls_proto_list) {
      if (util::strieq(supported.name, name)) {
        ok = true;
        break;
      }
    }
    if (!ok) {
      res |= supported.mask;
    }
  }
  return res;
}

SSL_CTX *create_ssl_context(const char *private_key_file, const char *cert_file
#ifdef HAVE_NEVERBLEED
                            ,
                            neverbleed_t *nb
#endif // HAVE_NEVERBLEED
                            ) {
  auto ssl_ctx = SSL_CTX_new(SSLv23_server_method());
  if (!ssl_ctx) {
    LOG(FATAL) << ERR_error_string(ERR_get_error(), nullptr);
    DIE();
  }

  constexpr auto ssl_opts =
      (SSL_OP_ALL & ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS) | SSL_OP_NO_SSLv2 |
      SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION |
      SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION | SSL_OP_SINGLE_ECDH_USE |
      SSL_OP_SINGLE_DH_USE | SSL_OP_CIPHER_SERVER_PREFERENCE;

  auto &tlsconf = get_config()->tls;

  SSL_CTX_set_options(ssl_ctx, ssl_opts | tlsconf.tls_proto_mask);

  const unsigned char sid_ctx[] = "shrpx";
  SSL_CTX_set_session_id_context(ssl_ctx, sid_ctx, sizeof(sid_ctx) - 1);
  SSL_CTX_set_session_cache_mode(ssl_ctx, SSL_SESS_CACHE_SERVER);

  if (!tlsconf.session_cache.memcached.host.empty()) {
    SSL_CTX_sess_set_new_cb(ssl_ctx, tls_session_new_cb);
    SSL_CTX_sess_set_get_cb(ssl_ctx, tls_session_get_cb);
  }

  SSL_CTX_set_timeout(ssl_ctx, tlsconf.session_timeout.count());

  const char *ciphers;
  if (!tlsconf.ciphers.empty()) {
    ciphers = tlsconf.ciphers.c_str();
  } else {
    ciphers = nghttp2::ssl::DEFAULT_CIPHER_LIST;
  }

  if (SSL_CTX_set_cipher_list(ssl_ctx, ciphers) == 0) {
    LOG(FATAL) << "SSL_CTX_set_cipher_list " << ciphers
               << " failed: " << ERR_error_string(ERR_get_error(), nullptr);
    DIE();
  }

#ifndef OPENSSL_NO_EC

  // Disabled SSL_CTX_set_ecdh_auto, because computational cost of
  // chosen curve is much higher than P-256.

  // #if OPENSSL_VERSION_NUMBER >= 0x10002000L
  //   SSL_CTX_set_ecdh_auto(ssl_ctx, 1);
  // #else // OPENSSL_VERSION_NUBMER < 0x10002000L
  // Use P-256, which is sufficiently secure at the time of this
  // writing.
  auto ecdh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
  if (ecdh == nullptr) {
    LOG(FATAL) << "EC_KEY_new_by_curv_name failed: "
               << ERR_error_string(ERR_get_error(), nullptr);
    DIE();
  }
  SSL_CTX_set_tmp_ecdh(ssl_ctx, ecdh);
  EC_KEY_free(ecdh);
// #endif // OPENSSL_VERSION_NUBMER < 0x10002000L

#endif // OPENSSL_NO_EC

  if (!tlsconf.dh_param_file.empty()) {
    // Read DH parameters from file
    auto bio = BIO_new_file(tlsconf.dh_param_file.c_str(), "r");
    if (bio == nullptr) {
      LOG(FATAL) << "BIO_new_file() failed: "
                 << ERR_error_string(ERR_get_error(), nullptr);
      DIE();
    }
    auto dh = PEM_read_bio_DHparams(bio, nullptr, nullptr, nullptr);
    if (dh == nullptr) {
      LOG(FATAL) << "PEM_read_bio_DHparams() failed: "
                 << ERR_error_string(ERR_get_error(), nullptr);
      DIE();
    }
    SSL_CTX_set_tmp_dh(ssl_ctx, dh);
    DH_free(dh);
    BIO_free(bio);
  }

  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_RELEASE_BUFFERS);
  if (!tlsconf.private_key_passwd.empty()) {
    SSL_CTX_set_default_passwd_cb(ssl_ctx, ssl_pem_passwd_cb);
    SSL_CTX_set_default_passwd_cb_userdata(ssl_ctx, (void *)get_config());
  }

#ifndef HAVE_NEVERBLEED
  if (SSL_CTX_use_PrivateKey_file(ssl_ctx, private_key_file,
                                  SSL_FILETYPE_PEM) != 1) {
    LOG(FATAL) << "SSL_CTX_use_PrivateKey_file failed: "
               << ERR_error_string(ERR_get_error(), nullptr);
  }
#else  // HAVE_NEVERBLEED
  std::array<char, NEVERBLEED_ERRBUF_SIZE> errbuf;
  if (neverbleed_load_private_key_file(nb, ssl_ctx, private_key_file,
                                       errbuf.data()) != 1) {
    LOG(FATAL) << "neverbleed_load_private_key_file failed: " << errbuf.data();
    DIE();
  }
#endif // HAVE_NEVERBLEED

  if (SSL_CTX_use_certificate_chain_file(ssl_ctx, cert_file) != 1) {
    LOG(FATAL) << "SSL_CTX_use_certificate_file failed: "
               << ERR_error_string(ERR_get_error(), nullptr);
    DIE();
  }
  if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
    LOG(FATAL) << "SSL_CTX_check_private_key failed: "
               << ERR_error_string(ERR_get_error(), nullptr);
    DIE();
  }
  if (tlsconf.client_verify.enabled) {
    if (!tlsconf.client_verify.cacert.empty()) {
      if (SSL_CTX_load_verify_locations(
              ssl_ctx, tlsconf.client_verify.cacert.c_str(), nullptr) != 1) {

        LOG(FATAL) << "Could not load trusted ca certificates from "
                   << tlsconf.client_verify.cacert << ": "
                   << ERR_error_string(ERR_get_error(), nullptr);
        DIE();
      }
      // It is heard that SSL_CTX_load_verify_locations() may leave
      // error even though it returns success. See
      // http://forum.nginx.org/read.php?29,242540
      ERR_clear_error();
      auto list = SSL_load_client_CA_file(tlsconf.client_verify.cacert.c_str());
      if (!list) {
        LOG(FATAL) << "Could not load ca certificates from "
                   << tlsconf.client_verify.cacert << ": "
                   << ERR_error_string(ERR_get_error(), nullptr);
        DIE();
      }
      SSL_CTX_set_client_CA_list(ssl_ctx, list);
    }
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE |
                                    SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       verify_callback);
  }
  SSL_CTX_set_tlsext_servername_callback(ssl_ctx, servername_callback);
  SSL_CTX_set_tlsext_ticket_key_cb(ssl_ctx, ticket_key_cb);
#ifndef OPENSSL_IS_BORINGSSL
  SSL_CTX_set_tlsext_status_cb(ssl_ctx, ocsp_resp_cb);
#endif // OPENSSL_IS_BORINGSSL
  SSL_CTX_set_info_callback(ssl_ctx, info_callback);

  // NPN advertisement
  SSL_CTX_set_next_protos_advertised_cb(ssl_ctx, next_proto_cb, nullptr);
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
  // ALPN selection callback
  SSL_CTX_set_alpn_select_cb(ssl_ctx, alpn_select_proto_cb, nullptr);
#endif // OPENSSL_VERSION_NUMBER >= 0x10002000L

  auto tls_ctx_data = new TLSContextData();
  tls_ctx_data->cert_file = cert_file;

  SSL_CTX_set_app_data(ssl_ctx, tls_ctx_data);

  return ssl_ctx;
}

namespace {
int select_h2_next_proto_cb(SSL *ssl, unsigned char **out,
                            unsigned char *outlen, const unsigned char *in,
                            unsigned int inlen, void *arg) {
  if (!util::select_h2(const_cast<const unsigned char **>(out), outlen, in,
                       inlen)) {
    return SSL_TLSEXT_ERR_NOACK;
  }

  return SSL_TLSEXT_ERR_OK;
}
} // namespace

namespace {
int select_h1_next_proto_cb(SSL *ssl, unsigned char **out,
                            unsigned char *outlen, const unsigned char *in,
                            unsigned int inlen, void *arg) {
  auto end = in + inlen;
  for (; in < end;) {
    if (util::streq(NGHTTP2_H1_1_ALPN, StringRef{in, in + (in[0] + 1)})) {
      *out = const_cast<unsigned char *>(in) + 1;
      *outlen = in[0];
      return SSL_TLSEXT_ERR_OK;
    }
    in += in[0] + 1;
  }

  return SSL_TLSEXT_ERR_NOACK;
}
} // namespace

namespace {
int select_next_proto_cb(SSL *ssl, unsigned char **out, unsigned char *outlen,
                         const unsigned char *in, unsigned int inlen,
                         void *arg) {
  auto conn = static_cast<Connection *>(SSL_get_app_data(ssl));
  switch (conn->proto) {
  case PROTO_HTTP1:
    return select_h1_next_proto_cb(ssl, out, outlen, in, inlen, arg);
  case PROTO_HTTP2:
    return select_h2_next_proto_cb(ssl, out, outlen, in, inlen, arg);
  default:
    return SSL_TLSEXT_ERR_NOACK;
  }
}
} // namespace

SSL_CTX *create_ssl_client_context(
#ifdef HAVE_NEVERBLEED
    neverbleed_t *nb,
#endif // HAVE_NEVERBLEED
    const StringRef &cacert, const StringRef &cert_file,
    const StringRef &private_key_file,
    int (*next_proto_select_cb)(SSL *s, unsigned char **out,
                                unsigned char *outlen, const unsigned char *in,
                                unsigned int inlen, void *arg)) {
  auto ssl_ctx = SSL_CTX_new(SSLv23_client_method());
  if (!ssl_ctx) {
    LOG(FATAL) << ERR_error_string(ERR_get_error(), nullptr);
    DIE();
  }

  constexpr auto ssl_opts = (SSL_OP_ALL & ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS) |
                            SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
                            SSL_OP_NO_COMPRESSION |
                            SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION;

  auto &tlsconf = get_config()->tls;

  SSL_CTX_set_options(ssl_ctx, ssl_opts | tlsconf.tls_proto_mask);

  const char *ciphers;
  if (!tlsconf.ciphers.empty()) {
    ciphers = tlsconf.ciphers.c_str();
  } else {
    ciphers = nghttp2::ssl::DEFAULT_CIPHER_LIST;
  }
  if (SSL_CTX_set_cipher_list(ssl_ctx, ciphers) == 0) {
    LOG(FATAL) << "SSL_CTX_set_cipher_list " << ciphers
               << " failed: " << ERR_error_string(ERR_get_error(), nullptr);
    DIE();
  }

  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_RELEASE_BUFFERS);

  if (SSL_CTX_set_default_verify_paths(ssl_ctx) != 1) {
    LOG(WARN) << "Could not load system trusted ca certificates: "
              << ERR_error_string(ERR_get_error(), nullptr);
  }

  if (!cacert.empty()) {
    if (SSL_CTX_load_verify_locations(ssl_ctx, cacert.c_str(), nullptr) != 1) {

      LOG(FATAL) << "Could not load trusted ca certificates from " << cacert
                 << ": " << ERR_error_string(ERR_get_error(), nullptr);
      DIE();
    }
  }

  if (!cert_file.empty()) {
    if (SSL_CTX_use_certificate_chain_file(ssl_ctx, cert_file.c_str()) != 1) {

      LOG(FATAL) << "Could not load client certificate from " << cert_file
                 << ": " << ERR_error_string(ERR_get_error(), nullptr);
      DIE();
    }
  }

  if (!private_key_file.empty()) {
#ifndef HAVE_NEVERBLEED
    if (SSL_CTX_use_PrivateKey_file(ssl_ctx, private_key_file.c_str(),
                                    SSL_FILETYPE_PEM) != 1) {
      LOG(FATAL) << "Could not load client private key from "
                 << private_key_file << ": "
                 << ERR_error_string(ERR_get_error(), nullptr);
      DIE();
    }
#else  // HAVE_NEVERBLEED
    std::array<char, NEVERBLEED_ERRBUF_SIZE> errbuf;
    if (neverbleed_load_private_key_file(nb, ssl_ctx, private_key_file.c_str(),
                                         errbuf.data()) != 1) {
      LOG(FATAL) << "neverbleed_load_private_key_file: could not load client "
                    "private key from " << private_key_file << ": "
                 << errbuf.data();
      DIE();
    }
#endif // HAVE_NEVERBLEED
  }

  // NPN selection callback.  This is required to set SSL_CTX because
  // OpenSSL does not offer SSL_set_next_proto_select_cb.
  SSL_CTX_set_next_proto_select_cb(ssl_ctx, next_proto_select_cb, nullptr);

  return ssl_ctx;
}

SSL *create_ssl(SSL_CTX *ssl_ctx) {
  auto ssl = SSL_new(ssl_ctx);
  if (!ssl) {
    LOG(ERROR) << "SSL_new() failed: " << ERR_error_string(ERR_get_error(),
                                                           nullptr);
    return nullptr;
  }

  return ssl;
}

ClientHandler *accept_connection(Worker *worker, int fd, sockaddr *addr,
                                 int addrlen, const UpstreamAddr *faddr) {
  char host[NI_MAXHOST];
  char service[NI_MAXSERV];
  int rv;

  if (addr->sa_family == AF_UNIX) {
    std::copy_n("localhost", sizeof("localhost"), host);
    service[0] = '\0';
  } else {
    rv = getnameinfo(addr, addrlen, host, sizeof(host), service,
                     sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV);
    if (rv != 0) {
      LOG(ERROR) << "getnameinfo() failed: " << gai_strerror(rv);

      return nullptr;
    }

    rv = util::make_socket_nodelay(fd);
    if (rv == -1) {
      LOG(WARN) << "Setting option TCP_NODELAY failed: errno=" << errno;
    }
  }
  SSL *ssl = nullptr;
  if (faddr->tls) {
    auto ssl_ctx = worker->get_sv_ssl_ctx();

    assert(ssl_ctx);

    ssl = create_ssl(ssl_ctx);
    if (!ssl) {
      return nullptr;
    }
    // Disable TLS session ticket if we don't have working ticket
    // keys.
    if (!worker->get_ticket_keys()) {
      SSL_set_options(ssl, SSL_OP_NO_TICKET);
    }
  }

  return new ClientHandler(worker, fd, ssl, host, service, addr->sa_family,
                           faddr);
}

bool tls_hostname_match(const StringRef &pattern, const StringRef &hostname) {
  auto ptWildcard = std::find(std::begin(pattern), std::end(pattern), '*');
  if (ptWildcard == std::end(pattern)) {
    return util::strieq(pattern, hostname);
  }

  auto ptLeftLabelEnd = std::find(std::begin(pattern), std::end(pattern), '.');
  auto wildcardEnabled = true;
  // Do case-insensitive match. At least 2 dots are required to enable
  // wildcard match. Also wildcard must be in the left-most label.
  // Don't attempt to match a presented identifier where the wildcard
  // character is embedded within an A-label.
  if (ptLeftLabelEnd == std::end(pattern) ||
      std::find(ptLeftLabelEnd + 1, std::end(pattern), '.') ==
          std::end(pattern) ||
      ptLeftLabelEnd < ptWildcard || util::istarts_with_l(pattern, "xn--")) {
    wildcardEnabled = false;
  }

  if (!wildcardEnabled) {
    return util::strieq(pattern, hostname);
  }

  auto hnLeftLabelEnd =
      std::find(std::begin(hostname), std::end(hostname), '.');
  if (hnLeftLabelEnd == std::end(hostname) ||
      !util::strieq(StringRef{ptLeftLabelEnd, std::end(pattern)},
                    StringRef{hnLeftLabelEnd, std::end(hostname)})) {
    return false;
  }
  // Perform wildcard match. Here '*' must match at least one
  // character.
  if (hnLeftLabelEnd - std::begin(hostname) <
      ptLeftLabelEnd - std::begin(pattern)) {
    return false;
  }
  return util::istarts_with(StringRef{std::begin(hostname), hnLeftLabelEnd},
                            StringRef{std::begin(pattern), ptWildcard}) &&
         util::iends_with(StringRef{std::begin(hostname), hnLeftLabelEnd},
                          StringRef{ptWildcard + 1, ptLeftLabelEnd});
}

namespace {
// if return value is not empty, StringRef.c_str() must be freed using
// OPENSSL_free().
StringRef get_common_name(X509 *cert) {
  auto subjectname = X509_get_subject_name(cert);
  if (!subjectname) {
    LOG(WARN) << "Could not get X509 name object from the certificate.";
    return StringRef{};
  }
  int lastpos = -1;
  for (;;) {
    lastpos = X509_NAME_get_index_by_NID(subjectname, NID_commonName, lastpos);
    if (lastpos == -1) {
      break;
    }
    auto entry = X509_NAME_get_entry(subjectname, lastpos);

    unsigned char *p;
    auto plen = ASN1_STRING_to_UTF8(&p, X509_NAME_ENTRY_get_data(entry));
    if (plen < 0) {
      continue;
    }
    if (std::find(p, p + plen, '\0') != p + plen) {
      // Embedded NULL is not permitted.
      continue;
    }
    if (plen == 0) {
      LOG(WARN) << "X509 name is empty";
      OPENSSL_free(p);
      continue;
    }

    return StringRef{p, static_cast<size_t>(plen)};
  }
  return StringRef{};
}
} // namespace

namespace {
int verify_numeric_hostname(X509 *cert, const StringRef &hostname,
                            const Address *addr) {
  const void *saddr;
  switch (addr->su.storage.ss_family) {
  case AF_INET:
    saddr = &addr->su.in.sin_addr;
    break;
  case AF_INET6:
    saddr = &addr->su.in6.sin6_addr;
    break;
  default:
    return -1;
  }

  auto altnames = static_cast<GENERAL_NAMES *>(
      X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
  if (altnames) {
    auto altnames_deleter = defer(GENERAL_NAMES_free, altnames);
    size_t n = sk_GENERAL_NAME_num(altnames);
    for (size_t i = 0; i < n; ++i) {
      auto altname = sk_GENERAL_NAME_value(altnames, i);
      if (altname->type != GEN_IPADD) {
        continue;
      }

      auto ip_addr = altname->d.iPAddress->data;
      if (!ip_addr) {
        continue;
      }
      size_t ip_addrlen = altname->d.iPAddress->length;

      if (addr->len == ip_addrlen && memcmp(saddr, ip_addr, ip_addrlen) == 0) {
        return 0;
      }
    }
  }

  auto cn = get_common_name(cert);
  if (cn.empty()) {
    return -1;
  }

  // cn is not NULL terminated
  auto rv = util::streq(hostname, cn);
  OPENSSL_free(const_cast<char *>(cn.c_str()));

  if (rv) {
    return 0;
  }

  return -1;
}
} // namespace

namespace {
int verify_hostname(X509 *cert, const StringRef &hostname,
                    const Address *addr) {
  if (util::numeric_host(hostname.c_str())) {
    return verify_numeric_hostname(cert, hostname, addr);
  }

  auto altnames = static_cast<GENERAL_NAMES *>(
      X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
  if (altnames) {
    auto altnames_deleter = defer(GENERAL_NAMES_free, altnames);
    size_t n = sk_GENERAL_NAME_num(altnames);
    for (size_t i = 0; i < n; ++i) {
      auto altname = sk_GENERAL_NAME_value(altnames, i);
      if (altname->type != GEN_DNS) {
        continue;
      }

      auto name = reinterpret_cast<char *>(ASN1_STRING_data(altname->d.ia5));
      if (!name) {
        continue;
      }

      auto len = ASN1_STRING_length(altname->d.ia5);
      if (len == 0) {
        continue;
      }
      if (std::find(name, name + len, '\0') != name + len) {
        // Embedded NULL is not permitted.
        continue;
      }

      if (name[len - 1] == '.') {
        --len;
        if (len == 0) {
          continue;
        }
      }

      if (tls_hostname_match(StringRef{name, static_cast<size_t>(len)},
                             hostname)) {
        return 0;
      }
    }
  }

  auto cn = get_common_name(cert);
  if (cn.empty()) {
    return -1;
  }

  if (cn[cn.size() - 1] == '.') {
    if (cn.size() == 1) {
      OPENSSL_free(const_cast<char *>(cn.c_str()));

      return -1;
    }
    cn = StringRef{cn.c_str(), cn.size() - 1};
  }

  auto rv = tls_hostname_match(cn, hostname);
  OPENSSL_free(const_cast<char *>(cn.c_str()));

  return rv ? 0 : -1;
}
} // namespace

int check_cert(SSL *ssl, const Address *addr, const StringRef &host) {
  auto cert = SSL_get_peer_certificate(ssl);
  if (!cert) {
    LOG(ERROR) << "No certificate found";
    return -1;
  }
  auto cert_deleter = defer(X509_free, cert);
  auto verify_res = SSL_get_verify_result(ssl);
  if (verify_res != X509_V_OK) {
    LOG(ERROR) << "Certificate verification failed: "
               << X509_verify_cert_error_string(verify_res);
    return -1;
  }

  if (verify_hostname(cert, host, addr) != 0) {
    LOG(ERROR) << "Certificate verification failed: hostname does not match";
    return -1;
  }
  return 0;
}

int check_cert(SSL *ssl, const DownstreamAddr *addr) {
  auto &backend_sni_name = get_config()->tls.backend_sni_name;

  auto hostname = !backend_sni_name.empty() ? StringRef(backend_sni_name)
                                            : StringRef(addr->host);
  return check_cert(ssl, &addr->addr, hostname);
}

CertLookupTree::CertLookupTree() {
  root_.ssl_ctx = nullptr;
  root_.str = nullptr;
  root_.first = root_.last = 0;
}

namespace {
// The |offset| is the index in the hostname we are examining.  We are
// going to scan from |offset| in backwards.
void cert_lookup_tree_add_cert(CertNode *node, SSL_CTX *ssl_ctx,
                               const char *hostname, size_t len, int offset) {
  int i, next_len = node->next.size();
  char c = hostname[offset];
  CertNode *cn = nullptr;
  for (i = 0; i < next_len; ++i) {
    cn = node->next[i].get();
    if (cn->str[cn->first] == c) {
      break;
    }
  }
  if (i == next_len) {
    if (c == '*') {
      // We assume hostname as wildcard hostname when first '*' is
      // encountered. Note that as per RFC 6125 (6.4.3), there are
      // some restrictions for wildcard hostname. We just ignore
      // these rules here but do the proper check when we do the
      // match.
      node->wildcard_certs.push_back({ssl_ctx, hostname, len});
      return;
    }

    int j;
    auto new_node = make_unique<CertNode>();
    new_node->str = hostname;
    new_node->first = offset;
    // If wildcard is found, set the region before it because we
    // don't include it in [first, last).
    for (j = offset; j >= 0 && hostname[j] != '*'; --j)
      ;
    new_node->last = j;
    if (j == -1) {
      new_node->ssl_ctx = ssl_ctx;
    } else {
      new_node->ssl_ctx = nullptr;
      new_node->wildcard_certs.push_back({ssl_ctx, hostname, len});
    }
    node->next.push_back(std::move(new_node));
    return;
  }

  int j;
  for (i = cn->first, j = offset;
       i > cn->last && j >= 0 && cn->str[i] == hostname[j]; --i, --j)
    ;
  if (i == cn->last) {
    if (j == -1) {
      // If the same hostname already exists, we don't overwrite
      // exiting ssl_ctx
      if (!cn->ssl_ctx) {
        cn->ssl_ctx = ssl_ctx;
      }
      return;
    }

    // The existing hostname is a suffix of this hostname.  Continue
    // matching at potion j.
    cert_lookup_tree_add_cert(cn, ssl_ctx, hostname, len, j);
    return;
  }

  {
    auto new_node = make_unique<CertNode>();
    new_node->ssl_ctx = cn->ssl_ctx;
    new_node->str = cn->str;
    new_node->first = i;
    new_node->last = cn->last;
    new_node->wildcard_certs.swap(cn->wildcard_certs);
    new_node->next.swap(cn->next);

    cn->next.push_back(std::move(new_node));
  }

  cn->last = i;
  if (j == -1) {
    // This hostname is a suffix of the existing hostname.
    cn->ssl_ctx = ssl_ctx;
    return;
  }

  // This hostname and existing one share suffix.
  cn->ssl_ctx = nullptr;
  cert_lookup_tree_add_cert(cn, ssl_ctx, hostname, len, j);
}
} // namespace

void CertLookupTree::add_cert(SSL_CTX *ssl_ctx, const StringRef &hostname) {
  if (hostname.empty()) {
    return;
  }
  // Copy hostname
  auto host_copy = make_unique<char[]>(hostname.size() + 1);
  std::copy(std::begin(hostname), std::end(hostname), host_copy.get());
  host_copy[hostname.size()] = '\0';
  util::inp_strlower(&host_copy[0], &host_copy[0] + hostname.size());

  cert_lookup_tree_add_cert(&root_, ssl_ctx, host_copy.get(), hostname.size(),
                            hostname.size() - 1);

  hosts_.push_back(std::move(host_copy));
}

namespace {
SSL_CTX *cert_lookup_tree_lookup(CertNode *node, const StringRef &hostname,
                                 int offset) {
  int i, j;
  for (i = node->first, j = offset;
       i > node->last && j >= 0 && node->str[i] == util::lowcase(hostname[j]);
       --i, --j)
    ;
  if (i != node->last) {
    return nullptr;
  }
  if (j == -1) {
    if (node->ssl_ctx) {
      // exact match
      return node->ssl_ctx;
    }

    // Do not perform wildcard-match because '*' must match at least
    // one character.
    return nullptr;
  }

  for (const auto &wildcert : node->wildcard_certs) {
    if (tls_hostname_match(StringRef{wildcert.hostname, wildcert.hostnamelen},
                           hostname)) {
      return wildcert.ssl_ctx;
    }
  }
  auto c = util::lowcase(hostname[j]);
  for (const auto &next_node : node->next) {
    if (next_node->str[next_node->first] == c) {
      return cert_lookup_tree_lookup(next_node.get(), hostname, j);
    }
  }
  return nullptr;
}
} // namespace

SSL_CTX *CertLookupTree::lookup(const StringRef &hostname) {
  if (hostname.empty()) {
    return nullptr;
  }
  return cert_lookup_tree_lookup(&root_, hostname, hostname.size() - 1);
}

int cert_lookup_tree_add_cert_from_file(CertLookupTree *lt, SSL_CTX *ssl_ctx,
                                        const char *certfile) {
  auto bio = BIO_new(BIO_s_file());
  if (!bio) {
    LOG(ERROR) << "BIO_new failed";
    return -1;
  }
  auto bio_deleter = defer(BIO_vfree, bio);
  if (!BIO_read_filename(bio, certfile)) {
    LOG(ERROR) << "Could not read certificate file '" << certfile << "'";
    return -1;
  }
  auto cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  if (!cert) {
    LOG(ERROR) << "Could not read X509 structure from file '" << certfile
               << "'";
    return -1;
  }
  auto cert_deleter = defer(X509_free, cert);

  auto altnames = static_cast<GENERAL_NAMES *>(
      X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
  if (altnames) {
    auto altnames_deleter = defer(GENERAL_NAMES_free, altnames);
    size_t n = sk_GENERAL_NAME_num(altnames);
    for (size_t i = 0; i < n; ++i) {
      auto altname = sk_GENERAL_NAME_value(altnames, i);
      if (altname->type != GEN_DNS) {
        continue;
      }

      auto name = reinterpret_cast<char *>(ASN1_STRING_data(altname->d.ia5));
      if (!name) {
        continue;
      }

      auto len = ASN1_STRING_length(altname->d.ia5);
      if (len == 0) {
        continue;
      }
      if (std::find(name, name + len, '\0') != name + len) {
        // Embedded NULL is not permitted.
        continue;
      }

      if (name[len - 1] == '.') {
        --len;
        if (len == 0) {
          continue;
        }
      }

      lt->add_cert(ssl_ctx, StringRef{name, static_cast<size_t>(len)});
    }
  }

  auto cn = get_common_name(cert);
  if (cn.empty()) {
    return 0;
  }

  if (cn[cn.size() - 1] == '.') {
    if (cn.size() == 1) {
      OPENSSL_free(const_cast<char *>(cn.c_str()));

      return 0;
    }

    cn = StringRef{cn.c_str(), cn.size() - 1};
  }

  lt->add_cert(ssl_ctx, cn);

  OPENSSL_free(const_cast<char *>(cn.c_str()));

  return 0;
}

bool in_proto_list(const std::vector<std::string> &protos,
                   const StringRef &needle) {
  for (auto &proto : protos) {
    if (util::streq(StringRef{proto}, needle)) {
      return true;
    }
  }
  return false;
}

bool upstream_tls_enabled() {
  const auto &faddrs = get_config()->conn.listener.addrs;
  return std::any_of(std::begin(faddrs), std::end(faddrs),
                     [](const UpstreamAddr &faddr) { return faddr.tls; });
}

SSL_CTX *setup_server_ssl_context(std::vector<SSL_CTX *> &all_ssl_ctx,
                                  CertLookupTree *cert_tree
#ifdef HAVE_NEVERBLEED
                                  ,
                                  neverbleed_t *nb
#endif // HAVE_NEVERBLEED
                                  ) {
  if (!upstream_tls_enabled()) {
    return nullptr;
  }

  auto &tlsconf = get_config()->tls;

  auto ssl_ctx = ssl::create_ssl_context(tlsconf.private_key_file.c_str(),
                                         tlsconf.cert_file.c_str()
#ifdef HAVE_NEVERBLEED
                                             ,
                                         nb
#endif // HAVE_NEVERBLEED
                                         );

  all_ssl_ctx.push_back(ssl_ctx);

  if (tlsconf.subcerts.empty()) {
    return ssl_ctx;
  }

  if (!cert_tree) {
    LOG(WARN) << "We have multiple additional certificates (--subcert), but "
                 "cert_tree is not given.  SNI may not work.";
    return ssl_ctx;
  }

  for (auto &keycert : tlsconf.subcerts) {
    auto ssl_ctx =
        ssl::create_ssl_context(keycert.first.c_str(), keycert.second.c_str()
#ifdef HAVE_NEVERBLEED
                                                           ,
                                nb
#endif // HAVE_NEVERBLEED
                                );
    all_ssl_ctx.push_back(ssl_ctx);
    if (ssl::cert_lookup_tree_add_cert_from_file(
            cert_tree, ssl_ctx, keycert.second.c_str()) == -1) {
      LOG(FATAL) << "Failed to add sub certificate.";
      DIE();
    }
  }

  if (ssl::cert_lookup_tree_add_cert_from_file(
          cert_tree, ssl_ctx, tlsconf.cert_file.c_str()) == -1) {
    LOG(FATAL) << "Failed to add default certificate.";
    DIE();
  }

  return ssl_ctx;
}

bool downstream_tls_enabled() {
  const auto &groups = get_config()->conn.downstream.addr_groups;

  return std::any_of(std::begin(groups), std::end(groups),
                     [](const DownstreamAddrGroupConfig &g) { return g.tls; });
}

SSL_CTX *setup_downstream_client_ssl_context(
#ifdef HAVE_NEVERBLEED
    neverbleed_t *nb
#endif // HAVE_NEVERBLEED
    ) {
  if (!downstream_tls_enabled()) {
    return nullptr;
  }

  auto &tlsconf = get_config()->tls;

  return ssl::create_ssl_client_context(
#ifdef HAVE_NEVERBLEED
      nb,
#endif // HAVE_NEVERBLEED
      StringRef{tlsconf.cacert}, StringRef{tlsconf.client.cert_file},
      StringRef{tlsconf.client.private_key_file}, select_next_proto_cb);
}

void setup_downstream_http2_alpn(SSL *ssl) {
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
  // ALPN advertisement
  auto alpn = util::get_default_alpn();
  SSL_set_alpn_protos(ssl, alpn.data(), alpn.size());
#endif // OPENSSL_VERSION_NUMBER >= 0x10002000L
}

void setup_downstream_http1_alpn(SSL *ssl) {
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
  // ALPN advertisement
  SSL_set_alpn_protos(ssl, NGHTTP2_H1_1_ALPN.byte(), NGHTTP2_H1_1_ALPN.size());
#endif // OPENSSL_VERSION_NUMBER >= 0x10002000L
}

CertLookupTree *create_cert_lookup_tree() {
  if (!upstream_tls_enabled() || get_config()->tls.subcerts.empty()) {
    return nullptr;
  }
  return new ssl::CertLookupTree();
}

namespace {
std::vector<uint8_t> serialize_ssl_session(SSL_SESSION *session) {
  auto len = i2d_SSL_SESSION(session, nullptr);
  auto buf = std::vector<uint8_t>(len);
  auto p = buf.data();
  i2d_SSL_SESSION(session, &p);

  return buf;
}
} // namespace

void try_cache_tls_session(DownstreamAddr *addr, SSL_SESSION *session,
                           ev_tstamp t) {
  auto &cache = addr->tls_session_cache;

  if (cache.last_updated + 1_min > t) {
    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "Cache for addr=" << util::to_numeric_addr(&addr->addr)
                << " is still host.  Not updating.";
    }
    return;
  }

  if (LOG_ENABLED(INFO)) {
    LOG(INFO) << "Update cache entry for SSL_SESSION=" << session
              << ", addr=" << util::to_numeric_addr(&addr->addr)
              << ", timestamp=" << std::fixed << std::setprecision(6) << t;
  }

  cache.session_data = serialize_ssl_session(session);
  cache.last_updated = t;
}

SSL_SESSION *reuse_tls_session(const DownstreamAddr *addr) {
  auto &cache = addr->tls_session_cache;

  if (cache.session_data.empty()) {
    return nullptr;
  }

  auto p = cache.session_data.data();
  return d2i_SSL_SESSION(nullptr, &p, cache.session_data.size());
}

} // namespace ssl

} // namespace shrpx
