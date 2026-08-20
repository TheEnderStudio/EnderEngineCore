#include <Audio/AudioSubsystem.hpp>
#include <Core/Log.hpp>
#include <Resource/ResourcesManager.hpp>

#include <phonon.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <combaseapi.h>
#include <Shlwapi.h>
#include <malloc.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>
#include <execution>
#include <winerror.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

#include <PxPhysicsAPI.h>

// ===================================================================
// PhysX → SteamAudio scene callbacks (any-hit, closest-hit, and batched)
// ===================================================================
using namespace physx;
static void* g_pxSceneForOcclusion = nullptr;

static void IPLCALL physxAnyHitCallback(const IPLRay* ray, IPLfloat32 minDist, IPLfloat32 maxDist, IPLuint8* occluded, void*)
{
	*occluded = 0;
	auto* scene = static_cast<PxScene*>(g_pxSceneForOcclusion);
	if (!scene || maxDist <= minDist) { *occluded = 1; return; }

	PxVec3 o(ray->origin.x, ray->origin.y, ray->origin.z);
	PxVec3 d(ray->direction.x, ray->direction.y, ray->direction.z);

	PxQueryFilterData fd; fd.flags = PxQueryFlag::eSTATIC;
	PxRaycastBuffer hit;
	if (scene->raycast(o, d, maxDist, hit, PxHitFlags(PxHitFlag::eMESH_BOTH_SIDES), fd))
		if (hit.block.distance >= minDist && hit.block.distance < maxDist * 0.95f)
			*occluded = 1;
}

static void IPLCALL physxClosestHitCallback(const IPLRay* ray, IPLfloat32 minDist, IPLfloat32 maxDist, IPLHit* hit, void*)
{
	hit->distance = INFINITY; hit->triangleIndex = -1; hit->objectIndex = -1;
	hit->materialIndex = -1; hit->normal.x = 0; hit->normal.y = 1; hit->normal.z = 0;
	hit->material = nullptr;

	auto* scene = static_cast<PxScene*>(g_pxSceneForOcclusion);
	if (!scene || maxDist <= minDist) return;

	PxVec3 o(ray->origin.x, ray->origin.y, ray->origin.z);
	PxVec3 d(ray->direction.x, ray->direction.y, ray->direction.z);

	PxQueryFilterData fd; fd.flags = PxQueryFlag::eSTATIC;
	PxRaycastBuffer buf;
	if (scene->raycast(o, d, maxDist, buf, PxHitFlags(PxHitFlag::eMESH_BOTH_SIDES), fd)) {
		if (buf.block.distance >= minDist && buf.block.distance < maxDist * 0.95f) {
			hit->distance = buf.block.distance;
			hit->normal.x = buf.block.normal.x;
			hit->normal.y = buf.block.normal.y;
			hit->normal.z = buf.block.normal.z;
		}
	}
}

static void IPLCALL physxBatchedAnyHitCallback(IPLint32 n, const IPLRay* rays, const IPLfloat32* minDs,
	const IPLfloat32* maxDs, IPLuint8* occ, void*)
{
	auto* scene = static_cast<PxScene*>(g_pxSceneForOcclusion);
	for (IPLint32 i = 0; i < n; i++) {
		occ[i] = 0;
		if (!scene || maxDs[i] <= minDs[i]) { occ[i] = 1; continue; }
		PxVec3 o(rays[i].origin.x, rays[i].origin.y, rays[i].origin.z);
		PxVec3 d(rays[i].direction.x, rays[i].direction.y, rays[i].direction.z);
		PxQueryFilterData fd; fd.flags = PxQueryFlag::eSTATIC;
		PxRaycastBuffer hit;
		if (scene->raycast(o, d, maxDs[i], hit, PxHitFlags(PxHitFlag::eMESH_BOTH_SIDES), fd))
			if (hit.block.distance >= minDs[i] && hit.block.distance < maxDs[i] * 0.95f)
				occ[i] = 1;
	}
}

static void IPLCALL physxBatchedClosestHitCallback(IPLint32 n, const IPLRay* rays, const IPLfloat32* minDs,
	const IPLfloat32* maxDs, IPLHit* hits, void*)
{
	auto* scene = static_cast<PxScene*>(g_pxSceneForOcclusion);
	for (IPLint32 i = 0; i < n; i++) {
		hits[i].distance = INFINITY; hits[i].triangleIndex = -1; hits[i].objectIndex = -1;
		hits[i].materialIndex = -1; hits[i].material = nullptr;
		hits[i].normal.x = 0; hits[i].normal.y = 1; hits[i].normal.z = 0;
		if (!scene || maxDs[i] <= minDs[i]) continue;
		PxVec3 o(rays[i].origin.x, rays[i].origin.y, rays[i].origin.z);
		PxVec3 d(rays[i].direction.x, rays[i].direction.y, rays[i].direction.z);
		PxQueryFilterData fd; fd.flags = PxQueryFlag::eSTATIC;
		PxRaycastBuffer buf;
		if (scene->raycast(o, d, maxDs[i], buf, PxHitFlags(PxHitFlag::eMESH_BOTH_SIDES), fd)) {
			if (buf.block.distance >= minDs[i] && buf.block.distance < maxDs[i] * 0.95f) {
				hits[i].distance = buf.block.distance;
				hits[i].normal.x = buf.block.normal.x;
				hits[i].normal.y = buf.block.normal.y;
				hits[i].normal.z = buf.block.normal.z;
			}
		}
	}
}

EE_NAMESPACE_AUDIO_BEGIN

// ===================================================================
// ActiveSound
// ===================================================================
struct ActiveSound {
	Vector<F32> samples;
	UInt32 channels, sampleRate;
	Vec3   position;
	F32    volume;
	UInt64 cursor;
	IPLPanningEffect panningEffect = nullptr;
	IPLDirectEffect  directEffect  = nullptr;
	IPLSource        iplSource     = nullptr;
	F32              occlusionFactor = 0.0f;
};

// ===================================================================
// InternalAudioCache
// ===================================================================
class InternalAudioCache {
private:
	using clock = std::chrono::high_resolution_clock;
	using timePoint = std::chrono::high_resolution_clock::time_point;
	struct CacheData {
		AudioClip audio;
		Size refCount = 0;
		timePoint storeTime = clock::now();
		CacheData(const AudioClip& clip) : audio(clip) {}
		CacheData(const AudioClip&& clip) : audio(std::move(clip)) {}
	};

public:
	EE_DEFAULT_CON_DES(InternalAudioCache);
	EE_NO_MOVE(InternalAudioCache);
	EE_NO_COPY(InternalAudioCache);

	constexpr static const Size CheckCleanLine = 2Ui64 * 1024 * 1024 * 1024;

	Optional<AudioClip*> read(const Path& path, AudioSubsystem* subsystem) {
		if (!subsystem) return std::nullopt;
		if (auto a = m_decoded.find(path); a != m_decoded.end()) { a->second.refCount++; return &a->second.audio; }
		auto r = subsystem->decode(path.string());
		if (r.isErr()) { EError("Audio decode failed: {} error={}", path.string(), ToString(r.error())); return std::nullopt; }
		CacheData cd(std::move(r.value())); auto [it, _] = m_decoded.insert_or_assign(path, std::move(cd));
		return &it->second.audio;
	}

	Optional<AudioClip*> read(const ResPath& path, AudioSubsystem* subsystem) {
		if (!subsystem) return std::nullopt;
		if (auto a = m_decoded.find(path.path); a != m_decoded.end()) { a->second.refCount++; return &a->second.audio; }
		auto r = subsystem->decode(path);
		if (r.isErr()) { EError("Audio decode failed: {} error={}", path.path.string(), ToString(r.error())); return std::nullopt; }
		CacheData cd(std::move(r.value())); auto [it, _] = m_decoded.insert_or_assign(path.path, std::move(cd));
		return &it->second.audio;
	}

	void clear() { m_decoded.clear(); }

	void update() {
		if (computeSize(m_decoded) <= CheckCleanLine) return;
		EDebug("Cleaning cache...");
		timePoint now = clock::now();
		auto pairs = Vector<std::pair<Path, CacheData>>(m_decoded.begin(), m_decoded.end());
		std::ranges::sort(pairs, std::greater{}, [&](const auto& p) {
			double dur = (double)(std::chrono::duration_cast<std::chrono::milliseconds>(now - p.second.storeTime).count()) / 1000.0;
			return p.second.refCount / dur;
			});
		m_decoded.clear();
		for (auto& pair : pairs) { m_decoded.insert(pair); if (computeSize(m_decoded) > CheckCleanLine) break; }
		EDebug("Cleaning done: {} MB", (double)computeSize(m_decoded) / 1024 / 1024);
	}

private:
	template <typename K, typename V>
	static Size computeSize(const HashMap<K, V>& t) {
		return t.size() * (sizeof(typename HashMap<K, V>::value_type) + sizeof(void*)) + t.bucket_count() * sizeof(void*);
	}
	HashMap<Path, CacheData> m_decoded{};
};

// ===================================================================
// Impl
// ===================================================================
struct AudioSubsystem::Impl {
	IPLContext       iplCtx = nullptr;
	IPLScene         iplScene = nullptr;
	IPLSimulator     iplSimulator = nullptr;
	IPLAudioSettings iplAudioSettings{};

	IAudioClient*       audioClient   = nullptr;
	IAudioRenderClient* renderClient  = nullptr;
	UINT32              bufferFrames  = 0;
	std::thread*        audioThread   = nullptr;
	std::atomic<bool>   running{false};
	std::atomic<bool>   shutdown{false};
	HANDLE              hEvent        = nullptr;

	Vec3   listenerPos = Vec3(0), listenerFwd = Vec3(0,0,-1), listenerUp = Vec3(0,1,0);
	UINT32 sampleRate = 48000;
	UINT32 bytesPerFrame = 8;
	UINT32 deviceChannels = 2;

	InternalAudioCache cache{};

	std::vector<ActiveSound> active;
	mutable std::mutex mutex;
};

// ===================================================================
// Lifecycle
// ===================================================================
AudioSubsystem::AudioSubsystem() : Subsystem("AudioSubsystem"), m_impl(std::make_unique<Impl>()) {}
AudioSubsystem::~AudioSubsystem() = default;

// ===================================================================
// Decode helpers
// ===================================================================
static Result<AudioClip, AudioError> decodeFromMemory(const Byte* data, size_t size);

Result<AudioClip, AudioError> AudioSubsystem::decode(const String& path) {
	if (!std::filesystem::exists(path)) { EError("Audio: file not found: {}", path); return AudioError::DecodeFailed; }

	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) { EError("Audio: cannot open {}", path); return AudioError::DecodeFailed; }
	size_t fileSize = (size_t)file.tellg();
	file.seekg(0);
	std::vector<BYTE> fileData(fileSize);
	file.read((char*)fileData.data(), fileSize);
	file.close();

	return decodeFromMemory(fileData.data(), fileData.size());
}

Result<AudioClip, AudioError> AudioSubsystem::decode(const ResPath& path) {
	auto& rm = ResourcesManager::getInstance();
	if (!rm.isMounted()) { EError("Audio: resources not mounted"); return AudioError::DecodeFailed; }
	auto r = rm.readFile(path);
	if (r.isErr()) { EError("Audio: resource read failed: {}, file name: '{}'", ToString(r.error()), path.path.string()); return AudioError::DecodeFailed; }
	auto& data = r.value();
	return decodeFromMemory(data.data(), data.size());
}

// Helper: decode from in-memory bytes (shared by both decode overloads)
static Result<AudioClip, AudioError> decodeFromMemory(const Byte* fileData, size_t fileSize) {
	AudioClip clip;
	if (!fileData || fileSize == 0) return AudioError::DecodeFailed;

	IStream* pStream = SHCreateMemStream(fileData, (UINT)fileSize);
	if (!pStream) { return AudioError::DecodeFailed; }

	IMFByteStream* byteStream = nullptr;
	HRESULT hr = MFCreateMFByteStreamOnStream(pStream, &byteStream);
	pStream->Release();
	if (FAILED(hr)) { return AudioError::DecodeFailed; }

	IMFSourceReader* reader = nullptr;
	hr = MFCreateSourceReaderFromByteStream(byteStream, nullptr, &reader);
	byteStream->Release();
	if (FAILED(hr)) { EError("Audio: SourceReaderFromByteStream failed 0x{:X}", (UInt32)hr); return AudioError::DecodeFailed; }

	IMFMediaType* pcm = nullptr; MFCreateMediaType(&pcm);
	pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
	pcm->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
	reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
	reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
	reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pcm);
	pcm->Release();

	IMFMediaType* outType = nullptr;
	reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &outType);
	outType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &clip.channels);
	outType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &clip.sampleRate);
	outType->Release();

	IMFSample* sample = nullptr; DWORD flags = 0;
	while (SUCCEEDED(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample)) && sample) {
		IMFMediaBuffer* buf = nullptr; sample->ConvertToContiguousBuffer(&buf);
		BYTE* data; DWORD len; buf->Lock(&data, nullptr, &len);
		clip.samples.insert(clip.samples.end(), (float*)data, (float*)data + len / sizeof(float));
		buf->Unlock(); buf->Release(); sample->Release();
	}
	reader->Release();
	return clip;
}

// ===================================================================
// onInitialize
// ===================================================================
Result<void, CoreError> AudioSubsystem::onInitialize() {
	auto& p = *m_impl;
	MFStartup(MF_VERSION);
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	IPLContextSettings cs{}; cs.version = STEAMAUDIO_VERSION;
	if (IPLerror err = iplContextCreate(&cs, &p.iplCtx); err) {
		EError("Audio: iplContextCreate failed: {}", (int)err); return CoreError::OperationFailed;
	}

	p.iplAudioSettings.samplingRate = 48000;
	p.iplAudioSettings.frameSize    = 1024;

	IMMDeviceEnumerator* enumerator = nullptr;
	if (HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator); FAILED(hr))
		{ EError("MMDeviceEnumerator failed: {:X}", hr); return CoreError::OperationFailed; }

	IMMDevice* device = nullptr;
	HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
	enumerator->Release();
	if (FAILED(hr)) { EError("GetDefaultAudioEndpoint failed: {:X}", hr); return CoreError::OperationFailed; }

	hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&p.audioClient);
	device->Release();
	if (FAILED(hr)) { EError("Activate IAudioClient failed: {:X}", hr); return CoreError::OperationFailed; }

	WAVEFORMATEX* pwf = nullptr;
	p.audioClient->GetMixFormat(&pwf);
	if (!pwf) { EError("GetMixFormat failed"); return CoreError::OperationFailed; }
	p.sampleRate = pwf->nSamplesPerSec;
	p.bytesPerFrame = pwf->nBlockAlign;
	p.deviceChannels = pwf->nChannels;
	hr = p.audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
		200000, 0, pwf, nullptr);
	CoTaskMemFree(pwf);
	if (FAILED(hr)) { EError("WASAPI Init failed: {:X}", hr); return CoreError::OperationFailed; }

	p.audioClient->GetBufferSize(&p.bufferFrames);
	p.audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&p.renderClient);
	p.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	p.audioClient->SetEventHandle(p.hEvent);

	p.iplAudioSettings.samplingRate = p.sampleRate;
	p.iplAudioSettings.frameSize    = 1024;
	EInfo("Audio: SteamAudio ready ({}Hz, {} frames)", p.sampleRate, p.bufferFrames);

	p.running.store(true);
	p.audioThread = new std::thread([implPtr = m_impl.get()]() {
		auto& p = *implPtr;
		p.audioClient->Start();

		const IPLint32 iplFrameSize = p.iplAudioSettings.frameSize;
		float* monoBuf  = (float*)_aligned_malloc(iplFrameSize * sizeof(float), 16);
		float* outLBuf  = (float*)_aligned_malloc(iplFrameSize * sizeof(float), 16);
		float* outRBuf  = (float*)_aligned_malloc(iplFrameSize * sizeof(float), 16);
		float* inChs[1]  = { monoBuf };
		float* outChs[2] = { outLBuf, outRBuf };
		IPLAudioBuffer tempIn  = { 1, iplFrameSize, inChs };
		IPLAudioBuffer tempOut = { 2, iplFrameSize, outChs };

		while (p.running.load()) {
			WaitForSingleObject(p.hEvent, 100);
			if (!p.running.load()) break;
			UINT32 padding; p.audioClient->GetCurrentPadding(&padding);
			UINT32 avail = p.bufferFrames - padding;
			if (avail == 0) continue;
			BYTE* rd; if (FAILED(p.renderClient->GetBuffer(avail, &rd))) continue;
			memset(rd, 0, avail * p.bytesPerFrame);

			{
				std::lock_guard<std::mutex> lk(p.mutex);
				for (auto it = p.active.begin(); it != p.active.end(); ) {
					if (it->cursor >= (UInt64)it->samples.size() / it->channels) {
						if (it->panningEffect) iplPanningEffectRelease(&it->panningEffect);
						if (it->directEffect)  iplDirectEffectRelease(&it->directEffect);
						if (it->iplSource)     iplSourceRelease(&it->iplSource);
						it = p.active.erase(it);
					} else { ++it; }
				}

				for (auto& s : p.active) {
					float dist = glm::length(s.position - p.listenerPos);
					float atten = std::max(0.0f, 1.0f - dist * 0.1f) * s.volume;

					IPLVector3 dir;
					{ Vec3 d = glm::normalize(s.position - p.listenerPos); dir = { d.x, d.y, d.z }; }

					UINT32 wasapiDone = 0;
					while (wasapiDone < avail) {
						UINT64 remain = (UINT64)s.samples.size() / s.channels - s.cursor;
						UINT32 chunkSize = (UINT32)std::min({ (UINT64)iplFrameSize, (UINT64)avail - wasapiDone, remain });
						if (chunkSize == 0) break;
						if (atten < 0.001f) { s.cursor += chunkSize; wasapiDone += chunkSize; continue; }

						const float* src = s.samples.data() + s.cursor * s.channels;
						for (UINT32 i = 0; i < chunkSize; i++)
							monoBuf[i] = src[i * (size_t)s.channels] * atten;
						for (UINT32 i = chunkSize; i < (UINT32)iplFrameSize; i++)
							monoBuf[i] = 0.0f;
						tempIn.numSamples  = iplFrameSize;
						tempOut.numSamples = iplFrameSize;

						IPLDirectEffectParams dp{};
						dp.flags = (IPLDirectEffectFlags)(IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION | IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
						dp.occlusion = s.occlusionFactor;
						dp.transmission[0] = 1.0f - s.occlusionFactor * 0.8f;
						dp.transmission[1] = 1.0f - s.occlusionFactor * 0.6f;
						dp.transmission[2] = 1.0f - s.occlusionFactor * 0.3f;
						iplDirectEffectApply(s.directEffect, &dp, &tempIn, &tempIn);

						IPLPanningEffectParams pp{};
						pp.direction = dir;
						iplPanningEffectApply(s.panningEffect, &pp, &tempIn, &tempOut);

						float* dst = (float*)rd + wasapiDone * 2;
						for (UINT32 i = 0; i < chunkSize; i++) {
							dst[i * 2 + 0] += outLBuf[i];
							dst[i * 2 + 1] += outRBuf[i];
						}
						s.cursor += chunkSize;
						wasapiDone += chunkSize;
					}
					if (atten < 0.001f) continue;
				}
			}
			p.renderClient->ReleaseBuffer(avail, 0);
		}
		p.audioClient->Stop();
		_aligned_free(monoBuf); _aligned_free(outLBuf); _aligned_free(outRBuf);
	});
	EInfo("WASAPI ready ({}Hz)", p.sampleRate);
	return {};
}

// ===================================================================
// onShutdown
// ===================================================================
void AudioSubsystem::onShutdown() {
	auto& p = *m_impl;
	if (p.shutdown.exchange(true)) return;
	if (p.running.load()) { p.running.store(false); SetEvent(p.hEvent);
		if (p.audioThread) { p.audioThread->join(); delete p.audioThread; p.audioThread = nullptr; } }
	if (p.hEvent) { CloseHandle(p.hEvent); p.hEvent = nullptr; }
	if (p.renderClient) { p.renderClient->Release(); p.renderClient = nullptr; }
	if (p.audioClient) { p.audioClient->Release(); p.audioClient = nullptr; }
	for (auto& s : p.active) {
		if (s.panningEffect) iplPanningEffectRelease(&s.panningEffect);
		if (s.directEffect)  iplDirectEffectRelease(&s.directEffect);
		if (s.iplSource)     iplSourceRelease(&s.iplSource);
	}
	p.active.clear();
	if (p.iplSimulator) { iplSimulatorRelease(&p.iplSimulator); p.iplSimulator = nullptr; }
	if (p.iplScene)     { iplSceneRelease(&p.iplScene); p.iplScene = nullptr; }
	if (p.iplCtx)       { iplContextRelease(&p.iplCtx); p.iplCtx = nullptr; }
	g_pxSceneForOcclusion = nullptr;
	CoUninitialize();
	MFShutdown();
	p.cache.clear();
}

// ===================================================================
// onUpdate — runs IPLSimulator to compute occlusion via PhysX callbacks
// ===================================================================
void AudioSubsystem::onUpdate(F64 d) {
	static F64 counter = 0;
	counter += d;
	if (counter > 10) m_impl->cache.update();

	auto& p = *m_impl;
	if (!p.iplSimulator) return;

	{
		std::lock_guard<std::mutex> lk(p.mutex);

		for (auto& s : p.active) {
			if (!s.iplSource) continue;
			IPLSimulationInputs inputs{};
			inputs.flags = IPL_SIMULATIONFLAGS_DIRECT;
			inputs.directFlags = IPL_DIRECTSIMULATIONFLAGS_OCCLUSION;
			inputs.source.origin.x    = s.position.x;
			inputs.source.origin.y    = s.position.y;
			inputs.source.origin.z    = s.position.z;
			inputs.source.ahead.x = 0; inputs.source.ahead.y = 0; inputs.source.ahead.z = 1;
			inputs.source.up.x    = 0; inputs.source.up.y    = 1; inputs.source.up.z    = 0;
			inputs.source.right.x = 1; inputs.source.right.y = 0; inputs.source.right.z = 0;
			inputs.occlusionType      = IPL_OCCLUSIONTYPE_RAYCAST;
			inputs.occlusionRadius    = 0.0f;
			inputs.numOcclusionSamples = 1;
			iplSourceSetInputs(s.iplSource, IPL_SIMULATIONFLAGS_DIRECT, &inputs);
		}

		IPLSimulationSharedInputs shared{};
		shared.listener.origin.x = p.listenerPos.x;
		shared.listener.origin.y = p.listenerPos.y;
		shared.listener.origin.z = p.listenerPos.z;
		shared.listener.ahead.x = p.listenerFwd.x;
		shared.listener.ahead.y = p.listenerFwd.y;
		shared.listener.ahead.z = p.listenerFwd.z;
		shared.listener.up.x    = p.listenerUp.x;
		shared.listener.up.y    = p.listenerUp.y;
		shared.listener.up.z    = p.listenerUp.z;
		shared.numRays   = 1;
		shared.numBounces = 0;
		shared.duration  = 1.0f;
		shared.order     = 0;
		shared.irradianceMinDistance = 1.0f;
		iplSimulatorSetSharedInputs(p.iplSimulator, IPL_SIMULATIONFLAGS_DIRECT, &shared);

		iplSimulatorCommit(p.iplSimulator);
		iplSimulatorRunDirect(p.iplSimulator);

		for (auto& s : p.active) {
			if (!s.iplSource) continue;
			IPLSimulationOutputs outputs{};
			iplSourceGetOutputs(s.iplSource, IPL_SIMULATIONFLAGS_DIRECT, &outputs);
			s.occlusionFactor = outputs.direct.occlusion;
		}
	}
}

// ===================================================================
// playFile
// ===================================================================
void AudioSubsystem::playFile(const String& path, const Vec3& pos, F32 vol) {
	auto r = m_impl->cache.read(path, this);
	if (!r.has_value() || !r.value()) return;
	auto& p = *m_impl;
	std::lock_guard<std::mutex> lk(p.mutex);
	ActiveSound s;
	s.samples    = (**r).samples;
	s.channels   = (**r).channels;
	s.sampleRate = (**r).sampleRate;
	s.position   = pos;
	s.volume     = vol;
	s.cursor     = 0;
	IPLPanningEffectSettings ps{}; ps.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
	iplPanningEffectCreate(p.iplCtx, &p.iplAudioSettings, &ps, &s.panningEffect);
	IPLDirectEffectSettings ds{}; ds.numChannels = 1;
	iplDirectEffectCreate(p.iplCtx, &p.iplAudioSettings, &ds, &s.directEffect);
	if (p.iplSimulator) {
		IPLSourceSettings ss{}; ss.flags = IPL_SIMULATIONFLAGS_DIRECT;
		iplSourceCreate(p.iplSimulator, &ss, &s.iplSource);
		iplSimulatorCommit(p.iplSimulator);
	}
	p.active.push_back(std::move(s));
}

void AudioSubsystem::playFile(const ResPath& path, const Vec3& pos, F32 vol) {
	auto r = m_impl->cache.read(path, this);
	if (!r.has_value() || !r.value()) return;
	auto& p = *m_impl;
	std::lock_guard<std::mutex> lk(p.mutex);
	ActiveSound s;
	s.samples    = (**r).samples;
	s.channels   = (**r).channels;
	s.sampleRate = (**r).sampleRate;
	s.position   = pos;
	s.volume     = vol;
	s.cursor     = 0;
	IPLPanningEffectSettings ps{}; ps.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
	iplPanningEffectCreate(p.iplCtx, &p.iplAudioSettings, &ps, &s.panningEffect);
	IPLDirectEffectSettings ds{}; ds.numChannels = 1;
	iplDirectEffectCreate(p.iplCtx, &p.iplAudioSettings, &ds, &s.directEffect);
	if (p.iplSimulator) {
		IPLSourceSettings ss{}; ss.flags = IPL_SIMULATIONFLAGS_DIRECT;
		iplSourceCreate(p.iplSimulator, &ss, &s.iplSource);
		iplSimulatorCommit(p.iplSimulator);
	}
	p.active.push_back(std::move(s));
}

void AudioSubsystem::setListener(const Vec3& pos, const Vec3& fwd, const Vec3& up) {
	m_impl->listenerPos = pos; m_impl->listenerFwd = fwd; m_impl->listenerUp = up;
}

// ===================================================================
// bindScene
// ===================================================================
void AudioSubsystem::bindScene(void* pxScene) {
	auto& p = *m_impl;
	if (g_pxSceneForOcclusion && p.iplSimulator) return;

	g_pxSceneForOcclusion = pxScene;

	IPLSceneSettings scs{};
	scs.type                    = IPL_SCENETYPE_CUSTOM;
	scs.anyHitCallback          = physxAnyHitCallback;
	scs.closestHitCallback      = physxClosestHitCallback;
	scs.batchedAnyHitCallback   = physxBatchedAnyHitCallback;
	scs.batchedClosestHitCallback = physxBatchedClosestHitCallback;
	scs.userData = nullptr;
	if (IPLerror err = iplSceneCreate(p.iplCtx, &scs, &p.iplScene); err) {
		EError("Audio: iplSceneCreate failed: {}", (int)err); return;
	}

	IPLSimulationSettings sims{};
	sims.flags       = IPL_SIMULATIONFLAGS_DIRECT;
	sims.sceneType   = IPL_SCENETYPE_CUSTOM;
	sims.maxNumSources    = 64;
	sims.numThreads       = 1;
	sims.samplingRate     = p.sampleRate;
	sims.frameSize        = p.iplAudioSettings.frameSize;
	sims.maxNumOcclusionSamples = 1;
	sims.maxNumRays       = 1;
	sims.numDiffuseSamples = 1;
	sims.maxDuration      = 1.0f;
	sims.maxOrder         = 0;
	if (IPLerror err = iplSimulatorCreate(p.iplCtx, &sims, &p.iplSimulator); err) {
		EError("Audio: iplSimulatorCreate failed: {}", (int)err); iplSceneRelease(&p.iplScene); p.iplScene = nullptr; return;
	}
	iplSimulatorSetScene(p.iplSimulator, p.iplScene);
	iplSimulatorCommit(p.iplSimulator);
	EInfo("Audio: SteamAudio scene + simulator bound ({}Hz, 64 sources)", p.sampleRate);
}

EE_NAMESPACE_AUDIO_END
