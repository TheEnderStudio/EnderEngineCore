#include <EngineExt/AdaptiveMusic.hpp>
#include <Engine/Core/Log.hpp>

#ifndef NOMINMAX
# define NOMINMAX
#endif

#include <Windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

namespace EnderEngine::Extensions::AdaptiveMusic {
	static String GetWin32ErrorMsg(DWORD dwMsgId, DWORD dwLangId = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT)) {
		String result;
		LPTSTR lpBuffer = NULL;
		static const DWORD dwFlags =
			FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS |
			FORMAT_MESSAGE_MAX_WIDTH_MASK;

		FormatMessage(
			dwFlags,
			NULL,
			dwMsgId,
			dwLangId,
			(LPTSTR)&lpBuffer,
			MAX_PATH,
			NULL
		);

		if (lpBuffer) {
			result = lpBuffer;
			LocalFree(lpBuffer);
		}

		return result;
	}

	using ::EnderEngine::Audio::AudioClip;

	class Player::Impl {
	private:
		struct Track {
			TrackHandle id;
			Sptr<const AudioClip> clip;
			F64 position;
			F32 volume;
			F32 pan;
			F32 speed;
			F32 pitch;
			bool playing;
			bool loop;
			bool finished = false;   // 自然播放到结尾
			F64 step;
		};

		mutable Mutex m_mutex;
		HashMap<TrackHandle, Track> m_tracks;
		TrackHandle m_nextId = 0;
		bool m_playing = false;
		F32 m_masterVolume = 1.0f;

		// WASAPI
		bool m_initialized = false;
		HANDLE m_audioEvent = nullptr;
		IAudioClient* m_audioClient = nullptr;
		IAudioRenderClient* m_renderClient = nullptr;
		UINT32 m_bufferSize = 0;
		UINT32 m_deviceSampleRate = 48000;
		UINT32 m_deviceChannels = 2;

		std::atomic<bool> m_stopThread{ false };
		std::jthread m_renderThread;

		// Helpers
		void computeStep(Track& track) const {
			if (!track.clip) return;
			track.step = track.speed * track.pitch *
				(static_cast<F64>(track.clip->sampleRate) / m_deviceSampleRate);
		}

		F32 readSampleLinear(const Vector<F32>& samples, F64 framePos, UInt32 channels, UInt32 channel) const {
			F64 index = framePos * channels + channel;
			Size idx0 = static_cast<Size>(std::floor(index));
			Size idx1 = idx0 + 1;
			F64 frac = index - idx0;
			if (idx1 >= samples.size()) {
				return (idx0 < samples.size()) ? samples[idx0] : 0.0f;
			}
			return static_cast<F32>(samples[idx0] * (1.0 - frac) + samples[idx1] * frac);
		}

		void mixToBuffer(F32* buffer, UINT32 numFrames) {
			std::fill(buffer, buffer + numFrames * m_deviceChannels, 0.0f);

			std::lock_guard<Mutex> lock(m_mutex);
			if (!m_playing) return;

			for (auto& kv : m_tracks) {
				Track& tr = kv.second;
				if (!tr.playing || !tr.clip) continue;

				const AudioClip& clip = *tr.clip;
				const UInt32 ch = clip.channels;
				const Size totalFrames = clip.samples.size() / ch;
				F64 pos = tr.position;
				F64 step = tr.step;

				if (pos >= static_cast<F64>(totalFrames)) {
					tr.playing = false;
					continue;
				}

				F32 vol = tr.volume * m_masterVolume;
				F32 panL = (1.0f - tr.pan) * 0.5f;
				F32 panR = (1.0f + tr.pan) * 0.5f;

				for (UINT32 i = 0; i < numFrames; ++i) {
					F64 curPos = pos + i * step;
					if (curPos >= static_cast<F64>(totalFrames)) {
						if (tr.loop) {
							curPos = std::fmod(curPos, static_cast<F64>(totalFrames));
						}
						else {
							tr.playing = false;
							tr.finished = true;
							break;
						}
					}

					F32 left, right;
					if (ch == 1) {
						F32 s = readSampleLinear(clip.samples, curPos, ch, 0);
						left = right = s;
					}
					else if (ch == 2) {
						left = readSampleLinear(clip.samples, curPos, ch, 0);
						right = readSampleLinear(clip.samples, curPos, ch, 1);
					}
					else {
						left = readSampleLinear(clip.samples, curPos, ch, 0);
						right = (ch > 1) ? readSampleLinear(clip.samples, curPos, ch, 1) : left;
					}

					Size outIdx = i * m_deviceChannels;
					buffer[outIdx + 0] += left * vol * panL;
					buffer[outIdx + 1] += right * vol * panR;
				}

				tr.position += numFrames * step;
				if (tr.position >= static_cast<double>(totalFrames) && !tr.loop) {
					tr.playing = false;
					tr.finished = true;
				}
			}
		}

	public:
		Impl() = default;
		~Impl() { shutdown(); }

		bool initialize() {
			std::lock_guard<Mutex> lock(m_mutex);
			if (m_initialized) return true;

			HRESULT hr;
			hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
				EError("COM initialized failed: 0x{:X} ({})", hr, GetWin32ErrorMsg(hr));
				return false;
			}

			IMMDeviceEnumerator* pEnumerator = nullptr;
			hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pEnumerator));
			if (FAILED(hr)) {
				EError("Device Enumerator creation failed: 0x{:X} ({})", hr, GetWin32ErrorMsg(hr));
				return false;
			}
			IMMDevice* pDevice = nullptr;
			hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
			pEnumerator->Release();
			if (FAILED(hr)) {
				EError("Failed to get Audio Device: 0x{:X} ({})", hr, GetWin32ErrorMsg(hr));
				return false;
			}

			IAudioClient* pAudioClient = nullptr;
			hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
			pDevice->Release();
			if (FAILED(hr)) {
				EError("Cannot activate the Audio Client: 0x{:X} ({})", hr, GetWin32ErrorMsg(hr));
				return false;
			}

			//WAVEFORMATEXTENSIBLE wfx = {};
			//wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
			//wfx.Format.nChannels = 2;
			//wfx.Format.nSamplesPerSec = 48000;
			//wfx.Format.wBitsPerSample = 32;
			//wfx.Format.nBlockAlign = 2 * 4;
			//wfx.Format.nAvgBytesPerSec = 48000 * wfx.Format.nBlockAlign;
			//wfx.Samples.wValidBitsPerSample = 32;
			//wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
			//WAVEFORMATEX* closest;
			//hr = pAudioClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, (WAVEFORMATEX*)&wfx, &closest);
			//EE_UNUSED(closest);
			//if (FAILED(hr)) {
			//	EError("Unsupported Audio Client: 0x{:X} ({})", hr, GetWin32ErrorMsg(hr));
			//	pAudioClient->Release();
			//	return false;
			//}

			HANDLE hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			if (!hEvent) {
				EError("Event creation failed: 0x{:X} ({})", hr, GetWin32ErrorMsg(hr));
				pAudioClient->Release();
				return false;
			}
			
			WAVEFORMATEX* pwf = nullptr;
			hr = pAudioClient->GetMixFormat(&pwf);
			if (!pwf || FAILED(hr)) { EError("GetMixFormat failed: 0x{:X} ({})", hr, GetWin32ErrorMsg(hr)); return false; }

			hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, pwf, nullptr);
			if (FAILED(hr)) {
				EError("Audio Client initialized failed: 0x{:X} ({})", hr, GetWin32ErrorMsg(hr));
				CloseHandle(hEvent);
				pAudioClient->Release();
				return false;
			}

			hr = pAudioClient->SetEventHandle(hEvent);
			if (FAILED(hr)) {
				EError("Cannot set event handle: 0x{:X} ({})", hr, GetWin32ErrorMsg(hr));
				CloseHandle(hEvent);
				pAudioClient->Release();
				return false;
			}

			UINT32 bufferSize = 0;
			hr = pAudioClient->GetBufferSize(&bufferSize);
			if (FAILED(hr)) {
				EError("Cannot get buffer size: 0x{:X} ({})", hr, GetWin32ErrorMsg(hr));
				CloseHandle(hEvent);
				pAudioClient->Release();
				return false;
			}

			IAudioRenderClient* pRenderClient = nullptr;
			hr = pAudioClient->GetService(IID_PPV_ARGS(&pRenderClient));
			if (FAILED(hr)) {
				EError("Cannot get Audio Render Client: 0x{:X} ({})", hr, GetWin32ErrorMsg(hr));
				CloseHandle(hEvent);
				pAudioClient->Release();
				return false;
			}

			hr = pAudioClient->Start();
			if (FAILED(hr)) {
				EError("Cannot start Audio Client: 0x{:X} ({})", hr, GetWin32ErrorMsg(hr));
				pRenderClient->Release();
				CloseHandle(hEvent);
				pAudioClient->Release();
				return false;
			}

			m_audioEvent = hEvent;
			m_audioClient = pAudioClient;
			m_renderClient = pRenderClient;
			m_bufferSize = bufferSize;
			m_deviceSampleRate = pwf->nSamplesPerSec;
			m_deviceChannels = pwf->nChannels;
			m_initialized = true;

			m_stopThread = false;
			m_renderThread = std::jthread(&Impl::renderLoop, this);
			return true;
		}

		void shutdown() {
			{
				std::lock_guard<Mutex> lock(m_mutex);
				if (!m_initialized) return;
				m_initialized = false;
				m_playing = false;
			}

			m_stopThread = true;
			if (m_renderThread.joinable()) {
				if (m_audioEvent) SetEvent(m_audioEvent);
				m_renderThread.join();
			}

			if (m_audioClient) {
				m_audioClient->Stop();
				m_audioClient->Release();
				m_audioClient = nullptr;
			}
			if (m_renderClient) {
				m_renderClient->Release();
				m_renderClient = nullptr;
			}
			if (m_audioEvent) {
				CloseHandle(m_audioEvent);
				m_audioEvent = nullptr;
			}
			CoUninitialize();
		}

		TrackHandle addTrack(const AudioClip& clip) {
			if (clip.sampleRate == 0 || clip.channels == 0 || clip.samples.empty())
				return InvalidTrackHandle;

			std::lock_guard<Mutex> lock(m_mutex);
			if (!m_initialized) return InvalidTrackHandle;

			TrackHandle id = m_nextId++;
			Track tr;
			tr.id = id;
			tr.clip = std::make_shared<AudioClip>(clip);
			tr.position = 0.0;
			tr.volume = 1.0f;
			tr.pan = 0.0f;
			tr.speed = 1.0f;
			tr.pitch = 1.0f;
			tr.playing = false;
			tr.loop = false;
			tr.step = 0.0;
			computeStep(tr);
			m_tracks.emplace(id, std::move(tr));
			return id;
		}

		void removeTrack(TrackHandle trackId) {
			std::lock_guard<Mutex> lock(m_mutex);
			m_tracks.erase(trackId);
		}

		bool play() {
			std::lock_guard<Mutex> lock(m_mutex);
			if (!m_initialized) return false;
			m_playing = true;

			for (auto& kv : m_tracks) {
				kv.second.playing = true;
			}
			return true;
		}

		bool pause() {
			std::lock_guard<Mutex> lock(m_mutex);
			if (!m_initialized) return false;
			m_playing = false;

			return true;
		}

		bool stop() {
			std::lock_guard<Mutex> lock(m_mutex);
			if (!m_initialized) return false;
			m_playing = false;
			for (auto& kv : m_tracks) {
				kv.second.playing = false;
				kv.second.position = 0.0;
				kv.second.finished = false;
			}
			return true;
		}

		bool isPlaying() const {
			std::lock_guard<Mutex> lock(m_mutex);
			return m_playing;
		}

		bool isTrackPlaying(TrackHandle trackId) const {
			std::lock_guard<Mutex> lock(m_mutex);
			auto it = m_tracks.find(trackId);
			return it != m_tracks.end() && it->second.playing;
		}

		bool isTrackFinished(TrackHandle trackId) const {
			std::lock_guard<Mutex> lock(m_mutex);
			auto it = m_tracks.find(trackId);
			return it != m_tracks.end() && it->second.finished;
		}

		void setMasterVolume(F32 volume) {
			std::lock_guard<Mutex> lock(m_mutex);
			m_masterVolume = Clamp(volume, 0.0f, 1.0f);
		}

		F32 getMasterVolume() const {
			std::lock_guard<Mutex> lock(m_mutex);
			return m_masterVolume;
		}

		void setTrackVolume(TrackHandle trackId, F32 volume) {
			std::lock_guard<Mutex> lock(m_mutex);
			auto it = m_tracks.find(trackId);
			if (it != m_tracks.end()) {
				it->second.volume = Clamp(volume, 0.0f, 1.0f);
			}
			else {
				EWarn("Cannot find track {}", trackId);
			}
		}

		void setTrackPan(TrackHandle trackId, F32 pan) {
			std::lock_guard<Mutex> lock(m_mutex);
			auto it = m_tracks.find(trackId);
			if (it != m_tracks.end()) {
				it->second.pan = Clamp(pan, -1.0f, 1.0f);
			}
			else {
				EWarn("Cannot find track {}", trackId);
			}
		}

		void setTrackSpeed(TrackHandle trackId, F32 speed) {
			std::lock_guard<Mutex> lock(m_mutex);
			auto it = m_tracks.find(trackId);
			if (it != m_tracks.end()) {
				it->second.speed = Clamp(speed, 0.5f, 2.0f);
				computeStep(it->second);
			}
			else {
				EWarn("Cannot find track {}", trackId);
			}
		}

		void setTrackPitch(TrackHandle trackId, F32 pitch) {
			std::lock_guard<Mutex> lock(m_mutex);
			auto it = m_tracks.find(trackId);
			if (it != m_tracks.end()) {
				it->second.pitch = Clamp(pitch, 0.5f, 2.0f);
				computeStep(it->second);
			}
			else {
				EWarn("Cannot find track {}", trackId);
			}
		}

		F32 getTrackVolume(TrackHandle trackId) const {
			std::lock_guard<Mutex> lock(m_mutex);
			const auto it = m_tracks.find(trackId);
			return it == m_tracks.end() ? 0.0f : it->second.volume;
		}

		F32 getTrackPan(TrackHandle trackId) const {
			std::lock_guard<Mutex> lock(m_mutex);
			const auto it = m_tracks.find(trackId);
			return it == m_tracks.end() ? 0.0f : it->second.pan;
		}

		F32 getTrackSpeed(TrackHandle trackId) const {
			std::lock_guard<Mutex> lock(m_mutex);
			const auto it = m_tracks.find(trackId);
			return it == m_tracks.end() ? 0.0f : it->second.speed;
		}

		F32 getTrackPitch(TrackHandle trackId) const {
			std::lock_guard<Mutex> lock(m_mutex);
			const auto it = m_tracks.find(trackId);
			return it == m_tracks.end() ? 0.0f : it->second.pitch;
		}

		void playTrack(TrackHandle trackId) {
			std::lock_guard<Mutex> lock(m_mutex);
			auto it = m_tracks.find(trackId);
			if (it != m_tracks.end()) {
				Track& tr = it->second;
				if (tr.clip && tr.clip->channels > 0) {
					const Size totalFrames = tr.clip->samples.size() / tr.clip->channels;
					if (tr.finished || tr.position >= static_cast<F64>(totalFrames)) {
						tr.position = 0.0;
					}
				}
				tr.finished = false;
				tr.playing = true;
				m_playing = true;   // 逐轨播放也需越过 mixToBuffer 的全局门闩
			}
			else {
				EWarn("Cannot find track {}", trackId);
			}
		}

		void pauseTrack(TrackHandle trackId) {
			std::lock_guard<Mutex> lock(m_mutex);
			auto it = m_tracks.find(trackId);
			if (it != m_tracks.end()) {
				it->second.playing = false;
			}
			else {
				EWarn("Cannot find track {}", trackId);
			}
		}

		void stopTrack(TrackHandle trackId) {
			std::lock_guard<Mutex> lock(m_mutex);
			auto it = m_tracks.find(trackId);
			if (it != m_tracks.end()) {
				it->second.playing = false;
				it->second.position = 0.0;
				it->second.finished = false;
			}
			else {
				EWarn("Cannot find track {}", trackId);
			}
		}

	private:
		void renderLoop() {
			HANDLE hEvent = m_audioEvent;
			if (!hEvent) return;

			DWORD taskIndex = 0;
			HANDLE hAvrt = AvSetMmThreadCharacteristics("Audio", &taskIndex);
			if (!hAvrt) {
				HRESULT hr = GetLastError();
				EWarn("SetMmThreadCharacteristics failed: 0x{:X} ({})", hr, GetWin32ErrorMsg(hr));
				hAvrt = nullptr;
			}

			while (!m_stopThread.load()) {
				DWORD wait = WaitForSingleObject(hEvent, INFINITE);
				if (wait != WAIT_OBJECT_0 || m_stopThread.load()) break;

				UINT32 padding = 0;
				HRESULT hr = m_audioClient->GetCurrentPadding(&padding);
				if (FAILED(hr)) break;
				UINT32 numFrames = m_bufferSize - padding;
				if (numFrames == 0) continue;

				BYTE* pData = nullptr;
				hr = m_renderClient->GetBuffer(numFrames, &pData);
				if (FAILED(hr)) break;

				F32* buffer = reinterpret_cast<F32*>(pData);
				mixToBuffer(buffer, numFrames);

				hr = m_renderClient->ReleaseBuffer(numFrames, 0);
				if (FAILED(hr)) break;
			}

			if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
		}
	};

	// Forwarding
	Player::Player() : m_impl(std::make_unique<Impl>()) {}
	Player::~Player() = default;

	bool Player::initialize() { return m_impl->initialize(); }
	TrackHandle Player::addTrack(const AudioClip& clip) { return m_impl->addTrack(clip); }
	void Player::removeTrack(TrackHandle trackId) { m_impl->removeTrack(trackId); }
	bool Player::play() { return m_impl->play(); }
	bool Player::pause() { return m_impl->pause(); }
	bool Player::stop() { return m_impl->stop(); }
	bool Player::isPlaying() const { return m_impl->isPlaying(); }
	bool Player::isTrackPlaying(TrackHandle id) const { return m_impl->isTrackPlaying(id); }
	bool Player::isTrackFinished(TrackHandle id) const { return m_impl->isTrackFinished(id); }

	void Player::setMasterVolume(F32 volume) { m_impl->setMasterVolume(volume); }
	F32 Player::getMasterVolume() const { return m_impl->getMasterVolume(); }

	void Player::setTrackVolume(TrackHandle trackId, F32 volume) { m_impl->setTrackVolume(trackId, volume); }
	void Player::setTrackPan(TrackHandle trackId, F32 pan) { m_impl->setTrackPan(trackId, pan); }
	void Player::setTrackSpeed(TrackHandle trackId, F32 speed) { m_impl->setTrackSpeed(trackId, speed); }
	void Player::setTrackPitch(TrackHandle trackId, F32 pitch) { m_impl->setTrackPitch(trackId, pitch); }
	F32 Player::getTrackVolume(TrackHandle trackId) const { return m_impl->getTrackVolume(trackId); }
	F32 Player::getTrackPan(TrackHandle trackId) const { return m_impl->getTrackPan(trackId); }
	F32 Player::getTrackSpeed(TrackHandle trackId) const { return m_impl->getTrackSpeed(trackId); }
	F32 Player::getTrackPitch(TrackHandle trackId) const { return m_impl->getTrackPitch(trackId); }

	void Player::playTrack(TrackHandle trackId) { m_impl->playTrack(trackId); }
	void Player::pauseTrack(TrackHandle trackId) { m_impl->pauseTrack(trackId); }
	void Player::stopTrack(TrackHandle trackId) { m_impl->stopTrack(trackId); }

} // namespace EnderEngine::Extensions::AdaptiveMusic