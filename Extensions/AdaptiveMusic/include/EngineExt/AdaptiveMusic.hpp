#pragma once

#ifndef EEEXT_A0_API
# ifdef EEEXT_A0_EXPORTS
#  define EEEXT_A0_API __declspec(dllexport)
# else
#  define EEEXT_A0_API __declspec(dllimport)
# endif
#endif

#include <Engine/Core/Extension.hpp>
#include <Engine/Core/ExtensionAPI.h>
#include <Engine/Core/Types.hpp>
#include <Engine/Audio/AudioTypes.hpp>

#include <variant>

#ifndef EE_WINDOWS
# error "This extension requires Windows"
#endif

namespace EnderEngine::Extensions::AdaptiveMusic {

using TrackHandle = UInt64;
static constexpr TrackHandle InvalidTrackHandle = 0xFFFFFFFFFFFFFFFFUi64;

// ===================================================================
// Player — WASAPI 多音轨渲染器
// ===================================================================
class EEEXT_A0_API Player {
public:
	Player();
	~Player();

	EE_NO_MOVE(Player);
	EE_NO_COPY(Player);

	/// @brief 初始化 WASAPI 渲染客户端并启动音频线程。
	bool initialize();

	/// @brief 添加一条音轨（拷贝 AudioClip），返回句柄。
	TrackHandle addTrack(const Audio::AudioClip& clip);
	/// @brief 移除一条音轨。
	void removeTrack(TrackHandle id);

	/// @brief 全局播放（所有音轨）。
	bool play();
	/// @brief 全局暂停。
	bool pause();
	/// @brief 全局停止（重置所有音轨到开头）。
	bool stop();
	/// @brief 是否处于全局播放状态。
	bool isPlaying() const;

	/// @brief 主音量 [0,1]。
	void setMasterVolume(F32 volume);
	F32 getMasterVolume() const;

	/// @brief 逐轨控制。
	void setTrackVolume(TrackHandle id, F32 volume);
	void setTrackPan(TrackHandle id, F32 pan);
	void setTrackSpeed(TrackHandle id, F32 speed);
	void setTrackPitch(TrackHandle id, F32 pitch);
	F32 getTrackVolume(TrackHandle id) const;
	F32 getTrackPan(TrackHandle id) const;
	F32 getTrackSpeed(TrackHandle id) const;
	F32 getTrackPitch(TrackHandle id) const;

	/// @brief 逐轨播放 / 暂停 / 停止。playTrack 会从开头重启已结束的音轨。
	void playTrack(TrackHandle id);
	void pauseTrack(TrackHandle id);
	void stopTrack(TrackHandle id);

	/// @brief 音轨是否正在播放。
	bool isTrackPlaying(TrackHandle id) const;
	/// @brief 音轨是否已自然播放到结尾（供 DSL WAIT 判定；stop/pause 会清除该标志）。
	bool isTrackFinished(TrackHandle id) const;

private:
	class Impl;
	Uptr<Impl> m_impl;
};

// ===================================================================
// HighLevelController — 自适应音乐 DSL 编译器 + 运行时
// ===================================================================

/// @brief SET 指令可修改的属性。
enum class SetProperty : UInt8 { Vol, Pan, Speed, Pitch };

class EEEXT_A0_API HighLevelController {
public:
	// ------------------------------------------------------------------
	// 指令集（编译后的中间表示）
	// ------------------------------------------------------------------
	enum class CmdType : UInt8 { Nop, Play, Stop, Fade, Set, Wait, Random, Halt };

	struct PlayCmdArgs {
		TrackHandle id = InvalidTrackHandle;
		F32 vol = 1.0f, pan = 0.0f, speed = 1.0f, pitch = 1.0f;
		F32 fadeInSec = 0.0f;   // >0 时从 0 淡入到 vol
	};
	struct StopCmdArgs {
		TrackHandle id = InvalidTrackHandle;
		F32 fadeOutSec = 0.0f;  // >0 时先淡出再停止
	};
	struct FadeCmdArgs {
		TrackHandle id = InvalidTrackHandle;
		bool fadeIn = false;
		F32 toVol = 0.0f;       // fadeIn 缺省 1.0，fadeOut 缺省 0.0
		F32 sec = 0.0f;
	};
	struct SetCmdArgs {
		TrackHandle id = InvalidTrackHandle;
		SetProperty prop = SetProperty::Vol;
		F32 value = 0.0f;
	};
	struct WaitCmdArgs {
		TrackHandle id = InvalidTrackHandle;
		bool untilFadeComplete = false;  // true = 等到该轨当前 fade 完成
	};
	struct RandomCmdArgs {
		Vector<UInt32> targets;          // 已解析的指令下标
	};

	using CmdArgs = std::variant<PlayCmdArgs, StopCmdArgs, FadeCmdArgs,
	                             SetCmdArgs, WaitCmdArgs, RandomCmdArgs>;
	struct Command {
		CmdType type = CmdType::Nop;
		CmdArgs args;
	};

	struct CompileError {
		enum class ErrorType : UInt8 {
			None,
			UnexpectedToken,   ///< 语法错误（缺参数 / 无法识别的 token / 数字非法）
			UnknownTrack,      ///< 引用了未 bindTrack 的音轨名
			UndefinedLabel,    ///< RANDOM 引用了不存在的标签
			DuplicateLabel,    ///< 标签重复定义
			EmptyScript,       ///< 脚本无任何指令
		};
		ErrorType type = ErrorType::None;
		UInt32 lineNum = 0;   ///< 出错行号（1 起）
		String message;
	};

	HighLevelController();
	~HighLevelController();

	EE_NO_MOVE(HighLevelController);
	EE_NO_COPY(HighLevelController);

	// ------------------------------------------------------------------
	// 绑定
	// ------------------------------------------------------------------

	/// @brief 绑定 Player（非持有）。执行指令前必须绑定。
	void attachPlayer(Player* player);
	/// @brief 获取绑定的 Player。
	Player* player() const;

	/// @brief 建立「音轨名 → TrackHandle」符号表。脚本中的名字在编译期解析为句柄。
	void bindTrack(const String& name, TrackHandle id);
	void unbindTrack(const String& name);
	void clearTracks();

	// ------------------------------------------------------------------
	// 编译
	// ------------------------------------------------------------------

	/**
	 * @brief 编译 DSL 脚本为可执行指令序列。
	 * @param script 脚本文本（行导向，';' 起注释）。
	 * @param outError 编译失败时接收错误信息（可为 nullptr）。
	 * @return true 编译成功；false 失败（错误写入 outError）。
	 */
	bool compile(const String& script, CompileError* outError = nullptr);

	// ------------------------------------------------------------------
	// 运行
	// ------------------------------------------------------------------

	/// @brief 从第一条指令开始执行（清空等待与进行中的 fade）。
	void start();
	/// @brief 停止脚本执行（等价于 HALT：音轨继续播放）。
	void stop();
	/// @brief 脚本是否正在运行（未执行到 HALT/末尾）。
	bool isRunning() const;

	/**
	 * @brief 每帧驱动：推进所有 fade，并处理 WAIT 阻塞与指令指针。
	 * @param dt 距上一帧秒数。
	 */
	void update(F32 dt);

private:
	class Impl;
	Uptr<Impl> m_impl;
};

// ===================================================================
// 扩展入口
// ===================================================================
class EEEXT_A0_API AdaptiveMusicExtension : public Extension {
public:
	AdaptiveMusicExtension() :
		Extension("EngineAdaptiveMusicExtension", Version(0, 1, 0, Guid(UUIDv4::UUID::fromStrFactory("AA175148-E5F2-4958-A646-85138C69B0A8"))), "sally4953") {}

	Player* createPlayer() const;
	void destroyPlayer(Player* player) const;
	HighLevelController* createHLController() const;
	void destroyHLController(HighLevelController* controller) const;
};

} // namespace EnderEngine::Extensions::AdaptiveMusic

extern "C" EEEXT_A0_API void* EE_EXT_GETEXTPTRFUNCNAME();
