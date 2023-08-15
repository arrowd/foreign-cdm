#include <cerrno>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <kj/main.h>
#include <capnp/rpc-twoparty.h>
#include <cdm/content_decryption_module.h>
#include "cdm.capnp.h"
#include "config.h"

class XAlloc {

  uint8_t* m_arena_start;
  uint32_t m_arena_size;
  uint8_t* m_position;

public:

  uint8_t* allocate(uint32_t nbytes) {
    auto cur_pos = m_position;
    auto new_pos = m_position + ((nbytes + 7) & ~7);
    KJ_ASSERT(new_pos < m_arena_start + m_arena_size, "out of mem");
    m_position = new_pos;
    return cur_pos;
  }

  uint32_t getOffset(uint8_t* position) {
    KJ_ASSERT(position >= m_arena_start && position < m_arena_start + m_arena_size, "out of bounds");
    return reinterpret_cast<uintptr_t>(position) - reinterpret_cast<uintptr_t>(m_arena_start);
  }

  void forget() {
    m_position = m_arena_start;
  }

  XAlloc(int fd, uint32_t arena_size) {
    void* p = mmap(nullptr, arena_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
      KJ_FAIL_SYSCALL("mmap", errno);
    }
    m_arena_start = reinterpret_cast<uint8_t*>(p);
    m_arena_size  = arena_size;
    m_position    = m_arena_start;
  }

  ~XAlloc() {
    if (m_arena_start != nullptr) {
      KJ_SYSCALL(munmap(m_arena_start, m_arena_size));
    }
  }

  XAlloc(XAlloc&& other) :
    m_arena_start(other.m_arena_start),
    m_arena_size (other.m_arena_size),
    m_position   (other.m_position)
  {
    other.m_arena_start = nullptr;
    other.m_arena_size  = 0;
  }

  KJ_DISALLOW_COPY(XAlloc);
};

class XBuffer: public cdm::Buffer {

  uint8_t* m_data;
  uint32_t m_capacity;
  uint32_t m_size;
  uint32_t m_offset;

 public:

  void Destroy() override {
    delete this;
  }

  uint32_t Capacity() const override {
    return m_capacity;
  }

  uint8_t* Data() override {
    return m_data;
  }

  void SetSize(uint32_t size) override {
    KJ_ASSERT(size <= m_capacity);
    m_size = size;
  }

  uint32_t Size() const override {
    return m_size;
  }

  XBuffer(uint32_t capacity, void* data) :
    m_data(static_cast<uint8_t*>(data)), m_capacity(capacity), m_size(capacity) {}

  ~XBuffer() {}
};

class XDecryptedBlock: public cdm::DecryptedBlock {

  cdm::Buffer* m_buffer    = nullptr;
  int64_t      m_timestamp = 0;

public:
  void SetDecryptedBuffer(cdm::Buffer* buffer) override {
    m_buffer = buffer;
  }

  cdm::Buffer* DecryptedBuffer() override {
    return m_buffer;
  }

  void SetTimestamp(int64_t timestamp) override {
    m_timestamp = timestamp;
  }

  int64_t Timestamp() const override {
    return m_timestamp;
  }

  XDecryptedBlock() {}
  ~XDecryptedBlock() {}
};

class XVideoFrame: public cdm::VideoFrame {

  cdm::VideoFormat m_format         = cdm::kUnknownVideoFormat;
  cdm::Size        m_size           = cdm::Size { .width = 0, .height = 0 };
  cdm::Buffer*     m_frame_buffer   = nullptr;
  uint32_t         m_kYPlane_offset = 0;
  uint32_t         m_kUPlane_offset = 0;
  uint32_t         m_kVPlane_offset = 0;
  uint32_t         m_kYPlane_stride = 0;
  uint32_t         m_kUPlane_stride = 0;
  uint32_t         m_kVPlane_stride = 0;
  int64_t          m_timestamp      = 0;

public:
  void SetFormat(cdm::VideoFormat format) override {
    m_format = format;
  }

  cdm::VideoFormat Format() const override {
    return m_format;
  }

  void SetSize(cdm::Size size) override {
    m_size = size;
  }

  cdm::Size Size() const override {
    return m_size;
  }

  void SetFrameBuffer(cdm::Buffer* frame_buffer) override {
    m_frame_buffer = frame_buffer;
  }

  cdm::Buffer* FrameBuffer() override {
    return m_frame_buffer;
  }

  void SetPlaneOffset(cdm::VideoPlane plane, uint32_t offset) override {
    switch (plane) {
      case cdm::kYPlane: m_kYPlane_offset = offset; break;
      case cdm::kUPlane: m_kUPlane_offset = offset; break;
      case cdm::kVPlane: m_kVPlane_offset = offset; break;
      default:
        KJ_UNREACHABLE;
    }
  }

  uint32_t PlaneOffset(cdm::VideoPlane plane) override {
    switch (plane) {
      case cdm::kYPlane: return m_kYPlane_offset;
      case cdm::kUPlane: return m_kUPlane_offset;
      case cdm::kVPlane: return m_kVPlane_offset;
      default:
        KJ_UNREACHABLE;
    }
  }

  void SetStride(cdm::VideoPlane plane, uint32_t stride) override {
    switch (plane) {
      case cdm::kYPlane: m_kYPlane_stride = stride; break;
      case cdm::kUPlane: m_kUPlane_stride = stride; break;
      case cdm::kVPlane: m_kVPlane_stride = stride; break;
      default:
        KJ_UNREACHABLE;
    }
  };

  uint32_t Stride(cdm::VideoPlane plane) override {
    switch (plane) {
      case cdm::kYPlane: return m_kYPlane_stride;
      case cdm::kUPlane: return m_kUPlane_stride;
      case cdm::kVPlane: return m_kVPlane_stride;
      default:
        KJ_UNREACHABLE;
    }
  }

  void SetTimestamp(int64_t timestamp) override {
    m_timestamp = timestamp;
  }

  int64_t Timestamp() const {
    return m_timestamp;
  }

  XVideoFrame() {}
  ~XVideoFrame() {}
};

static void decode_input_buffer(const InputBuffer2::Reader& source, cdm::InputBuffer_2* target) {

  auto data = source.getData();
  target->data      = data.begin();
  target->data_size = data.size();

  target->encryption_scheme = static_cast<cdm::EncryptionScheme>(source.getEncryptionScheme());

  auto key_id = source.getKeyId();
  target->key_id      = key_id.begin();
  target->key_id_size = key_id.size();

  auto iv = source.getIv();
  target->iv      = iv.begin();
  target->iv_size = iv.size();

  //TODO: get rid of malloc here
  auto num_subsamples = source.getSubsamples().size();
  auto subsamples     = new cdm::SubsampleEntry[num_subsamples];
  for (uint32_t i = 0; i < num_subsamples; i++) {
    subsamples[i].clear_bytes  = source.getSubsamples()[i].getClearBytes();
    subsamples[i].cipher_bytes = source.getSubsamples()[i].getCipherBytes();
  }
  target->num_subsamples = num_subsamples;
  target->subsamples     = subsamples;

  target->pattern = cdm::Pattern {
    .crypt_byte_block = source.getPattern().getCryptByteBlock(),
    .skip_byte_block  = source.getPattern().getSkipByteBlock()
  };

  target->timestamp = source.getTimestamp();
}

struct HostContext {
  kj::WaitScope* scope;
  XAlloc*        arena;
};

static thread_local struct HostContext host_ctx = HostContext { .scope = nullptr, .arena = nullptr };

static void set_host_context(kj::WaitScope* scope, XAlloc* arena) {
  KJ_ASSERT(host_ctx.scope == nullptr);
  KJ_ASSERT(host_ctx.arena == nullptr);
  host_ctx.scope = scope;
  host_ctx.arena = arena;
}

static void clear_host_context() {
  KJ_ASSERT(host_ctx.scope != nullptr);
  KJ_ASSERT(host_ctx.arena != nullptr);
  host_ctx.scope = nullptr;
  host_ctx.arena = nullptr;
}

class CdmProxyImpl final: public CdmProxy::Server {

  cdm::ContentDecryptionModule_10* m_cdm;
  kj::AutoCloseFd m_memfd;
  XAlloc m_allocator;

public:

  kj::Maybe<int> getFd() override {
    return m_memfd.get();
  }

  kj::Promise<void> initialize(InitializeContext context) override {
    return kj::startFiber(FIBER_STACK_SIZE, [context, this](kj::WaitScope& scope) mutable {
      KJ_DLOG(INFO, "initialize");
      set_host_context(&scope, &m_allocator);
      auto allow_distinctive_identifier = context.getParams().getAllowDistinctiveIdentifier();
      auto allow_persistent_state       = context.getParams().getAllowPersistentState();
      auto use_hw_secure_codecs         = context.getParams().getUseHwSecureCodecs();
      m_cdm->Initialize(allow_distinctive_identifier, allow_persistent_state, use_hw_secure_codecs);
      clear_host_context();
      KJ_DLOG(INFO, "exiting initialize");
    });
  }

  kj::Promise<void> setServerCertificate(SetServerCertificateContext context) override {
    return kj::startFiber(FIBER_STACK_SIZE, [context, this](kj::WaitScope& scope) mutable {
      KJ_DLOG(INFO, "setServerCertificate");
      set_host_context(&scope, &m_allocator);
      auto promise_id              = context.getParams().getPromiseId();
      auto server_certificate_data = context.getParams().getServerCertificateData();
      m_cdm->SetServerCertificate(promise_id, server_certificate_data.begin(), server_certificate_data.size());
      clear_host_context();
      KJ_DLOG(INFO, "exiting setServerCertificate");
    });
  }

  kj::Promise<void> createSessionAndGenerateRequest(CreateSessionAndGenerateRequestContext context) override {
    return kj::startFiber(FIBER_STACK_SIZE, [context, this](kj::WaitScope& scope) mutable {
      KJ_DLOG(INFO, "createSessionAndGenerateRequest");
      set_host_context(&scope, &m_allocator);
      auto promise_id     = context.getParams().getPromiseId();
      auto session_type   = context.getParams().getSessionType();
      auto init_data_type = context.getParams().getInitDataType();
      auto data           = context.getParams().getInitData();
      m_cdm->CreateSessionAndGenerateRequest(promise_id, static_cast<cdm::SessionType>(session_type), static_cast<cdm::InitDataType>(init_data_type), data.begin(), data.size());
      clear_host_context();
      KJ_DLOG(INFO, "exiting createSessionAndGenerateRequest");
    });
  }

  kj::Promise<void> updateSession(UpdateSessionContext context) override {
    return kj::startFiber(FIBER_STACK_SIZE, [context, this](kj::WaitScope& scope) mutable {
      KJ_DLOG(INFO, "updateSession");
      set_host_context(&scope, &m_allocator);
      auto promise_id = context.getParams().getPromiseId();
      auto session_id = context.getParams().getSessionId();
      auto response   = context.getParams().getResponse();
      m_cdm->UpdateSession(promise_id, session_id.begin(), session_id.size(), response.begin(), response.size());
      clear_host_context();
      KJ_DLOG(INFO, "exiting updateSession");
    });
  }

  kj::Promise<void> closeSession(CloseSessionContext context) override {
    return kj::startFiber(FIBER_STACK_SIZE, [context, this](kj::WaitScope& scope) mutable {
      KJ_DLOG(INFO, "closeSession");
      set_host_context(&scope, &m_allocator);
      auto promise_id = context.getParams().getPromiseId();
      auto session_id = context.getParams().getSessionId();
      m_cdm->CloseSession(promise_id, session_id.begin(), session_id.size());
      clear_host_context();
      KJ_DLOG(INFO, "exiting closeSession");
    });
  }

  kj::Promise<void> timerExpired(TimerExpiredContext context) override {
    return kj::startFiber(FIBER_STACK_SIZE, [context, this](kj::WaitScope& scope) mutable {
      KJ_DLOG(INFO, "timerExpired");
      set_host_context(&scope, &m_allocator);
      auto context_ = reinterpret_cast<void*>(context.getParams().getContext());
      m_cdm->TimerExpired(context_);
      clear_host_context();
      KJ_DLOG(INFO, "exiting timerExpired");
    });
  }

  kj::Promise<void> decrypt(DecryptContext context) override {
    return kj::startFiber(FIBER_STACK_SIZE, [context, this](kj::WaitScope& scope) mutable {
      KJ_DLOG(INFO, "decrypt");
      set_host_context(&scope, &m_allocator);

      cdm::InputBuffer_2 encrypted_buffer;
      decode_input_buffer(context.getParams().getEncryptedBuffer(), &encrypted_buffer);

      m_allocator.forget();

      XDecryptedBlock block;
      cdm::Status status = m_cdm->Decrypt(encrypted_buffer, static_cast<cdm::DecryptedBlock*>(&block));

      delete encrypted_buffer.subsamples;

      if (status == cdm::kSuccess) {
        auto target = context.getResults().getDecryptedBuffer();
        target.getBuffer().setOffset(m_allocator.getOffset(block.DecryptedBuffer()->Data()));
        target.getBuffer().setSize(block.DecryptedBuffer()->Size());
        target.setTimestamp(block.Timestamp());
      }

      if (block.DecryptedBuffer() != nullptr) {
        block.DecryptedBuffer()->Destroy();
      }

      context.getResults().setStatus(status);

      clear_host_context();
      KJ_DLOG(INFO, "exiting decrypt");
    });
  }

  kj::Promise<void> initializeVideoDecoder(InitializeVideoDecoderContext context) override {
    return kj::startFiber(FIBER_STACK_SIZE, [context, this](kj::WaitScope& scope) mutable {
      KJ_DLOG(INFO, "initializeVideoDecoder");
      set_host_context(&scope, &m_allocator);
      cdm::VideoDecoderConfig_2 video_decoder_config;
      video_decoder_config.codec             = static_cast<cdm::VideoCodec>(context.getParams().getVideoDecoderConfig().getCodec());
      video_decoder_config.profile           = static_cast<cdm::VideoCodecProfile>(context.getParams().getVideoDecoderConfig().getProfile());
      video_decoder_config.format            = static_cast<cdm::VideoFormat>(context.getParams().getVideoDecoderConfig().getFormat());
      video_decoder_config.coded_size.width  = context.getParams().getVideoDecoderConfig().getCodedSize().getWidth();
      video_decoder_config.coded_size.height = context.getParams().getVideoDecoderConfig().getCodedSize().getHeight();
      kj::ArrayPtr extra_data                = context.getParams().getVideoDecoderConfig().getExtraData();
      video_decoder_config.extra_data        = const_cast<uint8_t*>(extra_data.begin()); // somehow this field is non-const
      video_decoder_config.extra_data_size   = extra_data.size();
      video_decoder_config.encryption_scheme = static_cast<cdm::EncryptionScheme>(context.getParams().getVideoDecoderConfig().getEncryptionScheme());

      cdm::Status status = m_cdm->InitializeVideoDecoder(video_decoder_config);

      context.getResults().setStatus(status);
      clear_host_context();
      KJ_DLOG(INFO, "exiting initializeVideoDecoder");
    });
  }

  kj::Promise<void> deinitializeDecoder(DeinitializeDecoderContext context) override {
    return kj::startFiber(FIBER_STACK_SIZE, [context, this](kj::WaitScope& scope) mutable {
      KJ_DLOG(INFO, "deinitializeDecoder");
      set_host_context(&scope, &m_allocator);
      auto decoder_type = static_cast<cdm::StreamType>(context.getParams().getDecoderType());
      m_cdm->DeinitializeDecoder(decoder_type);
      clear_host_context();
      KJ_DLOG(INFO, "exiting deinitializeDecoder");
    });
  }

  kj::Promise<void> resetDecoder(ResetDecoderContext context) override {
    return kj::startFiber(FIBER_STACK_SIZE, [context, this](kj::WaitScope& scope) mutable {
      KJ_DLOG(INFO, "resetDecoder");
      set_host_context(&scope, &m_allocator);
      auto decoder_type = static_cast<cdm::StreamType>(context.getParams().getDecoderType());
      m_cdm->ResetDecoder(decoder_type);
      clear_host_context();
      KJ_DLOG(INFO, "exiting resetDecoder");
    });
  }

  kj::Promise<void> decryptAndDecodeFrame(DecryptAndDecodeFrameContext context) override {
    return kj::startFiber(FIBER_STACK_SIZE, [context, this](kj::WaitScope& scope) mutable {
      KJ_DLOG(INFO, "decryptAndDecodeFrame");
      set_host_context(&scope, &m_allocator);

      cdm::InputBuffer_2 encrypted_buffer;
      decode_input_buffer(context.getParams().getEncryptedBuffer(), &encrypted_buffer);

      m_allocator.forget();

      XVideoFrame frame;
      cdm::Status status = m_cdm->DecryptAndDecodeFrame(encrypted_buffer, static_cast<cdm::VideoFrame*>(&frame));

      delete encrypted_buffer.subsamples;

      if (status == cdm::kSuccess) {
        auto target = context.getResults().getVideoFrame();
        target.setFormat(frame.Format());
        target.getSize().setWidth (frame.Size().width);
        target.getSize().setHeight(frame.Size().height);

        target.getFrameBuffer().setOffset(m_allocator.getOffset(frame.FrameBuffer()->Data()));
        target.getFrameBuffer().setSize(frame.FrameBuffer()->Size());

        target.setKYPlaneOffset(frame.PlaneOffset(cdm::kYPlane));
        target.setKUPlaneOffset(frame.PlaneOffset(cdm::kUPlane));
        target.setKVPlaneOffset(frame.PlaneOffset(cdm::kVPlane));
        target.setKYPlaneStride(frame.Stride(cdm::kYPlane));
        target.setKUPlaneStride(frame.Stride(cdm::kUPlane));
        target.setKVPlaneStride(frame.Stride(cdm::kVPlane));
        target.setTimestamp(frame.Timestamp());
      }

      if (frame.FrameBuffer() != nullptr) {
        frame.FrameBuffer()->Destroy();
      }

      context.getResults().setStatus(status);

      clear_host_context();
      KJ_DLOG(INFO, "exiting decryptAndDecodeFrame");
    });
  }

  kj::Promise<void> onQueryOutputProtectionStatus(OnQueryOutputProtectionStatusContext context) override {
    return kj::startFiber(FIBER_STACK_SIZE, [context, this](kj::WaitScope& scope) mutable {
      KJ_DLOG(INFO, "onQueryOutputProtectionStatus");
      set_host_context(&scope, &m_allocator);
      auto result                 = context.getParams().getResult();
      auto link_mask              = context.getParams().getLinkMask();
      auto output_protection_mask = context.getParams().getOutputProtectionMask();
      m_cdm->OnQueryOutputProtectionStatus(static_cast<cdm::QueryResult>(result), link_mask, output_protection_mask);
      clear_host_context();
      KJ_DLOG(INFO, "exiting onQueryOutputProtectionStatus");
    });
  }

  CdmProxyImpl(cdm::ContentDecryptionModule_10* cdm, kj::AutoCloseFd memfd, XAlloc allocator) :
    m_cdm(cdm), m_memfd(kj::mv(memfd)), m_allocator(kj::mv(allocator)) {}

  ~CdmProxyImpl() {}
};

class HostWrapper: public cdm::Host_10 {

  HostProxy::Client m_host;

public:

  cdm::Buffer* Allocate(uint32_t capacity) override {
    return static_cast<cdm::Buffer*>(new XBuffer(capacity, host_ctx.arena->allocate(capacity)));
  }

  void SetTimer(int64_t delay_ms, void* context) override {
    KJ_DLOG(INFO, "SetTimer", delay_ms, context);
    auto request = m_host.setTimerRequest();
    request.setDelayMs(delay_ms);
    request.setContext(reinterpret_cast<uint64_t>(context));
    request.send().wait(*host_ctx.scope);
    KJ_DLOG(INFO, "exiting SetTimer");
  }

  cdm::Time GetCurrentWallTime() override {
    struct timeval tv;
    struct timezone tz = {0, 0};
    KJ_SYSCALL(gettimeofday(&tv, &tz));
    return static_cast<double>(tv.tv_sec) + tv.tv_usec / 1000000.0;
  }

  void OnInitialized(bool success) override {
    KJ_DLOG(INFO, "OnInitialized", success);
    auto request = m_host.onInitializedRequest();
    request.setSuccess(success);
    request.send().wait(*host_ctx.scope);
    KJ_DLOG(INFO, "exiting OnInitialized");
  }

  void OnResolveKeyStatusPromise(uint32_t promise_id, cdm::KeyStatus key_status) override {
    KJ_UNIMPLEMENTED("OnResolveKeyStatusPromise");
  }

  void OnResolveNewSessionPromise(uint32_t promise_id, const char* session_id, uint32_t session_id_size) override {
    KJ_DLOG(INFO, "OnResolveNewSessionPromise", promise_id, session_id, session_id_size);
    auto request = m_host.onResolveNewSessionPromiseRequest();
    request.setPromiseId(promise_id);
    request.setSessionId(kj::StringPtr(session_id, session_id_size));
    request.send().wait(*host_ctx.scope);
    KJ_DLOG(INFO, "exiting OnResolveNewSessionPromise");
  }

  void OnResolvePromise(uint32_t promise_id) override {
    KJ_DLOG(INFO, "OnResolvePromise", promise_id);
    auto request = m_host.onResolvePromiseRequest();
    request.setPromiseId(promise_id);
    request.send().wait(*host_ctx.scope);
    KJ_DLOG(INFO, "exiting OnResolvePromise");
  }

  void OnRejectPromise(uint32_t promise_id, cdm::Exception exception, uint32_t system_code, const char* error_message, uint32_t error_message_size) override {
    KJ_UNIMPLEMENTED("OnRejectPromise");
  }

  void OnSessionMessage(const char* session_id, uint32_t session_id_size, cdm::MessageType message_type, const char* message, uint32_t message_size) override {
    KJ_DLOG(INFO, "OnSessionMessage", session_id, session_id_size, message_type, message, message_size);
    auto request = m_host.onSessionMessageRequest();
    request.setSessionId(kj::StringPtr(session_id, session_id_size));
    request.setMessageType(message_type);
    request.setMessage(kj::StringPtr(message, message_size));
    request.send().wait(*host_ctx.scope);
    KJ_DLOG(INFO, "exiting OnSessionMessage");
  }

  void OnSessionKeysChange(const char* session_id, uint32_t session_id_size, bool has_additional_usable_key, const cdm::KeyInformation* keys_info, uint32_t keys_info_count) override {
    KJ_DLOG(INFO, "OnSessionKeysChange", session_id, session_id_size, has_additional_usable_key, keys_info, keys_info_count);
    auto request = m_host.onSessionKeysChangeRequest();
    request.setSessionId(kj::StringPtr(session_id, session_id_size));
    request.setHasAdditionalUsableKey(has_additional_usable_key);
    auto keys_info_builder = request.initKeysInfo(keys_info_count);
    for (uint32_t i = 0; i < keys_info_count; i++) {
      keys_info_builder[i].setKeyId(kj::ArrayPtr(keys_info[i].key_id, keys_info[i].key_id_size));
      keys_info_builder[i].setStatus(keys_info[i].status);
      keys_info_builder[i].setSystemCode(keys_info[i].system_code);
    }
    request.send().wait(*host_ctx.scope);
    KJ_DLOG(INFO, "exiting OnSessionKeysChange");
  }

  void OnExpirationChange(const char* session_id, uint32_t session_id_size, cdm::Time new_expiry_time) override {
    KJ_DLOG(INFO, "OnExpirationChange", session_id, session_id_size, new_expiry_time);
    auto request = m_host.onExpirationChangeRequest();
    request.setSessionId(kj::StringPtr(session_id, session_id_size));
    request.setNewExpiryTime(new_expiry_time);
    request.send().wait(*host_ctx.scope);
    KJ_DLOG(INFO, "exiting OnExpirationChange");
  }

  void OnSessionClosed(const char* session_id, uint32_t session_id_size) override {
    KJ_DLOG(INFO, "OnSessionClosed", session_id, session_id_size);
    auto request = m_host.onSessionClosedRequest();
    request.setSessionId(kj::StringPtr(session_id, session_id_size));
    request.send().wait(*host_ctx.scope);
    KJ_DLOG(INFO, "exiting OnSessionClosed");
  }

  void SendPlatformChallenge(const char* service_id, uint32_t service_id_size, const char* challenge, uint32_t challenge_size) override {
    KJ_UNIMPLEMENTED("SendPlatformChallenge");
  }

  void EnableOutputProtection(uint32_t desired_protection_mask) override {
    KJ_UNIMPLEMENTED("EnableOutputProtection");
  }

  void QueryOutputProtectionStatus() override {
    KJ_DLOG(INFO, "QueryOutputProtectionStatus");
    auto request = m_host.queryOutputProtectionStatusRequest();
    request.send().wait(*host_ctx.scope);
    KJ_DLOG(INFO, "exiting QueryOutputProtectionStatus");
  }

  void OnDeferredInitializationDone(cdm::StreamType stream_type, cdm::Status decoder_status) override {
    KJ_UNIMPLEMENTED("OnDeferredInitializationDone");
  }

  cdm::FileIO* CreateFileIO(cdm::FileIOClient* client) override {
    KJ_UNIMPLEMENTED("CreateFileIO");
  }

  void RequestStorageId(uint32_t version) override {
    KJ_UNIMPLEMENTED("RequestStorageId");
  }

  HostWrapper(HostProxy::Client&& host) : m_host(host) {}

  ~HostWrapper() noexcept {
    KJ_ASSERT(0);
  }
};

typedef void (*InitializeCdmModuleFunc)();
//~ typedef void (*DeinitializeCdmModuleFunc)();
typedef void* (*CreateCdmInstanceFunc)(int, const char*, uint32_t, GetCdmHostFunc, void*);
typedef const char* (*GetCdmVersionFunc)();

InitializeCdmModuleFunc   init_cdm_mod_func    = nullptr;
//~ DeinitializeCdmModuleFunc deinit_cdm_mod_func  = nullptr;
CreateCdmInstanceFunc     create_cdm_inst_func = nullptr;
GetCdmVersionFunc         get_cdm_ver_func     = nullptr;

static void* get_cdm_host(int host_interface_version, void* user_data) {
  KJ_DLOG(INFO, "get_cdm_host", host_interface_version, user_data);
  KJ_ASSERT(host_interface_version == 10);
  return user_data;
}

class CdmWorkerImpl final: public CdmWorker::Server {

  bool cdm_initialized = false;

public:

  kj::Promise<void> createCdmInstance(CreateCdmInstanceContext context) override {
    return kj::startFiber(FIBER_STACK_SIZE, [context, this](kj::WaitScope& scope) mutable {

      auto cdm_interface_version = context.getParams().getCdmInterfaceVersion();
      auto key_system            = context.getParams().getKeySystem();
      auto host_proxy            = context.getParams().getHostProxy();

      KJ_DLOG(INFO, "createCdmInstance", cdm_interface_version, key_system);
      KJ_ASSERT(cdm_interface_version == 10);

      if (!cdm_initialized) {
        KJ_LOG(INFO, "cdm version", get_cdm_ver_func());
        init_cdm_mod_func();
        cdm_initialized = true;
      }

      int fd;
      KJ_SYSCALL(fd = syscall(SYS_memfd_create, "decrypted buffers", 0));
      kj::AutoCloseFd memfd(fd);

      KJ_SYSCALL(ftruncate(memfd.get(), SHMEM_ARENA_SIZE));
      //TODO: seal memfd?

      XAlloc allocator(memfd.get(), SHMEM_ARENA_SIZE);

      //TODO: somebody is supposed to dispose of the host object
      void* host = reinterpret_cast<void*>(new HostWrapper(kj::mv(host_proxy)));

      set_host_context(&scope, &allocator);
      void* cdm  = create_cdm_inst_func(cdm_interface_version, key_system.begin(), key_system.size(), get_cdm_host, host);
      clear_host_context();
      KJ_ASSERT(cdm != nullptr);

      context.getResults().setCdmProxy(kj::heap<CdmProxyImpl>(reinterpret_cast<cdm::ContentDecryptionModule_10*>(cdm), kj::mv(memfd), kj::mv(allocator)));

      KJ_DLOG(INFO, "exiting createCdmInstance");
    });
  }

  kj::Promise<void> getCdmVersion(GetCdmVersionContext context) override {
    context.getResults().setVersion(get_cdm_ver_func());
    return kj::READY_NOW;
  }
};

int main(int argc, char* argv[]) {

  kj::TopLevelProcessContext context(argv[0]);
  context.increaseLoggingVerbosity();

  char* cdm_path = getenv("FCDM_CDM_SO_PATH");
  if (cdm_path == nullptr) {
    KJ_LOG(FATAL, "FCDM_CDM_SO_PATH is not set");
    exit(EXIT_FAILURE);
  }

  void* cdm = dlopen(cdm_path, RTLD_LAZY);
  KJ_ASSERT(cdm != nullptr);

#define SS(X) #X
#define S(X) SS(X)
  init_cdm_mod_func = (InitializeCdmModuleFunc)dlsym(cdm, S(INITIALIZE_CDM_MODULE));
#undef SS
#undef S
  KJ_ASSERT(init_cdm_mod_func != nullptr);

  //~ deinit_cdm_mod_func = (DeinitializeCdmModuleFunc)dlsym(cdm, "DeinitializeCdmModule");
  //~ KJ_ASSERT(deinit_cdm_mod_func != nullptr);

  create_cdm_inst_func = (CreateCdmInstanceFunc)dlsym(cdm, "CreateCdmInstance");
  KJ_ASSERT(create_cdm_inst_func != nullptr);

  get_cdm_ver_func = (GetCdmVersionFunc)dlsym(cdm, "GetCdmVersion");
  KJ_ASSERT(get_cdm_ver_func != nullptr);

  KJ_LOG(INFO, "started");

  if (argc != 2) {
    KJ_LOG(FATAL, "wrong number of args");
    exit(EXIT_FAILURE);
  }

  errno = 0;
  intmax_t socket_fd = strtoimax(argv[1], nullptr, 10);
  KJ_ASSERT(errno != ERANGE && errno != EINVAL);

  auto io = kj::setupAsyncIo();
  capnp::TwoPartyServer server(kj::heap<CdmWorkerImpl>());
  server.accept(io.lowLevelProvider->wrapUnixSocketFd(socket_fd), 1 /* maxFdsPerMessage */);

  class ErrorHandlerImpl: public kj::TaskSet::ErrorHandler {
  public:
    void taskFailed(kj::Exception&& exception) override {
      throw exception; // ?
    }
  };

  ErrorHandlerImpl errorHandler;
  kj::TaskSet tasks(errorHandler);

  tasks.add(server.drain().then([]() -> void {
    KJ_LOG(INFO, "exiting...");
    exit(EXIT_SUCCESS);
  }));

  kj::NEVER_DONE.wait(io.waitScope);
  return 0;
}