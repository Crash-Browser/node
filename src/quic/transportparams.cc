#if HAVE_OPENSSL && NODE_OPENSSL_HAS_QUIC

#include "transportparams.h"
#include <env-inl.h>
#include <memory_tracker-inl.h>
#include <node_sockaddr-inl.h>
#include <util-inl.h>
#include <v8.h>
#include "bindingdata.h"
#include "defs.h"
#include "endpoint.h"
#include "session.h"
#include "tokens.h"

namespace node {

using v8::ArrayBuffer;
using v8::Just;
using v8::Local;
using v8::Maybe;
using v8::Nothing;
using v8::Object;
using v8::Value;

namespace quic {

const TransportParams::Options TransportParams::Options::kDefault = {};

TransportParams::Config::Config(Side side,
                                const CID& ocid,
                                const CID& retry_scid)
    : side(side), ocid(ocid), retry_scid(retry_scid) {}

Maybe<TransportParams::Options> TransportParams::Options::From(
    Environment* env, Local<Value> value) {
  if (value.IsEmpty()) {
    THROW_ERR_INVALID_ARG_TYPE(env, "options must be an object");
    return Nothing<Options>();
  }

  Options options;
  auto& state = BindingData::Get(env);

  if (value->IsUndefined()) {
    return Just<Options>(options);
  }

  if (!value->IsObject()) {
    THROW_ERR_INVALID_ARG_TYPE(env, "options must be an object");
    return Nothing<Options>();
  }

  auto params = value.As<Object>();

#define SET(name)                                                              \
  SetOption<TransportParams::Options, &TransportParams::Options::name>(        \
      env, &options, params, state.name##_string())

  if (!SET(initial_max_stream_data_bidi_local) ||
      !SET(initial_max_stream_data_bidi_remote) ||
      !SET(initial_max_stream_data_uni) || !SET(initial_max_data) ||
      !SET(initial_max_streams_bidi) || !SET(initial_max_streams_uni) ||
      !SET(max_idle_timeout) || !SET(active_connection_id_limit) ||
      !SET(ack_delay_exponent) || !SET(max_ack_delay) ||
      !SET(max_datagram_frame_size) || !SET(disable_active_migration)) {
    return Nothing<Options>();
  }

#undef SET

  return Just<Options>(options);
}

void TransportParams::Options::MemoryInfo(MemoryTracker* tracker) const {
  if (preferred_address_ipv4.has_value()) {
    tracker->TrackField("preferred_address_ipv4",
                        preferred_address_ipv4.value());
  }
  if (preferred_address_ipv6.has_value()) {
    tracker->TrackField("preferred_address_ipv6",
                        preferred_address_ipv6.value());
  }
}

TransportParams::TransportParams(Type type) : type_(type), ptr_(&params_) {}

TransportParams::TransportParams(Type type, const ngtcp2_transport_params* ptr)
    : type_(type), ptr_(ptr) {}

TransportParams::TransportParams(const Config& config, const Options& options)
    : TransportParams(Type::ENCRYPTED_EXTENSIONS) {
  ngtcp2_transport_params_default(&params_);
  params_.active_connection_id_limit = options.active_connection_id_limit;
  params_.initial_max_stream_data_bidi_local =
      options.initial_max_stream_data_bidi_local;
  params_.initial_max_stream_data_bidi_remote =
      options.initial_max_stream_data_bidi_remote;
  params_.initial_max_stream_data_uni = options.initial_max_stream_data_uni;
  params_.initial_max_streams_bidi = options.initial_max_streams_bidi;
  params_.initial_max_streams_uni = options.initial_max_streams_uni;
  params_.initial_max_data = options.initial_max_data;
  params_.max_idle_timeout = options.max_idle_timeout * NGTCP2_SECONDS;
  params_.max_ack_delay = options.max_ack_delay;
  params_.ack_delay_exponent = options.ack_delay_exponent;
  params_.max_datagram_frame_size = options.max_datagram_frame_size;
  params_.disable_active_migration = options.disable_active_migration ? 1 : 0;
  params_.preferred_address_present = 0;
  params_.stateless_reset_token_present = 0;
  params_.retry_scid_present = 0;

  if (config.side == Side::SERVER) {
    // For the server side, the original dcid is always set.
    CHECK(config.ocid);
    params_.original_dcid = config.ocid;

    // The retry_scid is only set if the server validated a retry token.
    if (config.retry_scid) {
      params_.retry_scid = config.retry_scid;
      params_.retry_scid_present = 1;
    }
  }

  if (options.preferred_address_ipv4.has_value())
    SetPreferredAddress(options.preferred_address_ipv4.value());

  if (options.preferred_address_ipv6.has_value())
    SetPreferredAddress(options.preferred_address_ipv6.value());
}

TransportParams::TransportParams(Type type, const ngtcp2_vec& vec)
    : TransportParams(type) {
  int ret = ngtcp2_decode_transport_params(
      &params_,
      static_cast<ngtcp2_transport_params_type>(type),
      vec.base,
      vec.len);

  if (ret != 0) {
    ptr_ = nullptr;
    error_ = QuicError::ForNgtcp2Error(ret);
  }
}

Store TransportParams::Encode(Environment* env) {
  if (ptr_ == nullptr) {
    error_ = QuicError::ForNgtcp2Error(NGTCP2_INTERNAL_ERROR);
    return Store();
  }

  // Preflight to see how much storage we'll need.
  ssize_t size = ngtcp2_encode_transport_params(
      nullptr, 0, static_cast<ngtcp2_transport_params_type>(type_), &params_);

  DCHECK_GT(size, 0);

  auto result = ArrayBuffer::NewBackingStore(env->isolate(), size);

  auto ret = ngtcp2_encode_transport_params(
      static_cast<uint8_t*>(result->Data()),
      size,
      static_cast<ngtcp2_transport_params_type>(type_),
      &params_);

  if (ret != 0) {
    error_ = QuicError::ForNgtcp2Error(ret);
    return Store();
  }

  return Store(std::move(result), static_cast<size_t>(size));
}

void TransportParams::SetPreferredAddress(const SocketAddress& address) {
  DCHECK(ptr_ == &params_);
  params_.preferred_address_present = 1;
  switch (address.family()) {
    case AF_INET: {
      const sockaddr_in* src =
          reinterpret_cast<const sockaddr_in*>(address.data());
      memcpy(params_.preferred_address.ipv4_addr,
             &src->sin_addr,
             sizeof(params_.preferred_address.ipv4_addr));
      params_.preferred_address.ipv4_port = address.port();
      return;
    }
    case AF_INET6: {
      const sockaddr_in6* src =
          reinterpret_cast<const sockaddr_in6*>(address.data());
      memcpy(params_.preferred_address.ipv6_addr,
             &src->sin6_addr,
             sizeof(params_.preferred_address.ipv6_addr));
      params_.preferred_address.ipv6_port = address.port();
      return;
    }
  }
  UNREACHABLE();
}

void TransportParams::GenerateSessionTokens(Session* session) {
  if (session->is_server()) {
    GenerateStatelessResetToken(session->endpoint(), session->config_.scid);
    GeneratePreferredAddressToken(session);
  }
}

void TransportParams::GenerateStatelessResetToken(const Endpoint& endpoint,
                                                  const CID& cid) {
  DCHECK(ptr_ == &params_);
  DCHECK(cid);
  params_.stateless_reset_token_present = 1;
  endpoint.GenerateNewStatelessResetToken(params_.stateless_reset_token, cid);
}

void TransportParams::GeneratePreferredAddressToken(Session* session) {
  DCHECK(ptr_ == &params_);
  if (params_.preferred_address_present) {
    session->config_.preferred_address_cid = session->new_cid();
    params_.preferred_address.cid = session->config_.preferred_address_cid;
    auto& endpoint = session->endpoint();
    endpoint.AssociateStatelessResetToken(
        endpoint.GenerateNewStatelessResetToken(
            params_.preferred_address.stateless_reset_token,
            session->config_.preferred_address_cid),
        session);
  }
}

TransportParams::Type TransportParams::type() const {
  return type_;
}

TransportParams::operator const ngtcp2_transport_params&() const {
  DCHECK_NOT_NULL(ptr_);
  return *ptr_;
}

TransportParams::operator const ngtcp2_transport_params*() const {
  DCHECK_NOT_NULL(ptr_);
  return ptr_;
}

TransportParams::operator bool() const {
  return ptr_ != nullptr;
}

const QuicError& TransportParams::error() const {
  return error_;
}

void TransportParams::Initialize(Environment* env,
                                 v8::Local<v8::Object> target) {
  NODE_DEFINE_CONSTANT(target, DEFAULT_MAX_STREAM_DATA);
  NODE_DEFINE_CONSTANT(target, DEFAULT_MAX_DATA);
  NODE_DEFINE_CONSTANT(target, DEFAULT_MAX_IDLE_TIMEOUT);
  NODE_DEFINE_CONSTANT(target, DEFAULT_MAX_STREAMS_BIDI);
  NODE_DEFINE_CONSTANT(target, DEFAULT_MAX_STREAMS_UNI);
  NODE_DEFINE_CONSTANT(target, DEFAULT_ACTIVE_CONNECTION_ID_LIMIT);
}

}  // namespace quic
}  // namespace node

#endif  // HAVE_OPENSSL && NODE_OPENSSL_HAS_QUIC
