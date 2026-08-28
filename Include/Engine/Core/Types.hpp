#pragma once

#include "Macros.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <array>
#include <vector>
#include <unordered_map>
#include <map>
#include <concepts>
#include <memory>
#include <set>
#include <unordered_set>
#include <optional>
#include <mutex>
#include <variant>
#include <stack>
#include <filesystem>
#include <list>
#include <shared_mutex>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/compatibility.hpp>

EE_NAMESPACE_BEGIN

using Byte = uint8_t;
using UInt8 = uint8_t;
using Int8 = int8_t;
using UInt16 = uint16_t;
using Int16 = int16_t;
using UInt32 = uint32_t;
using Int32 = int32_t;
using UInt64 = uint64_t;
using Int64 = int64_t;
using Size = size_t;
using F32 = float;
using F64 = double;

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using IVec2 = glm::ivec2;
using IVec3 = glm::ivec3;
using IVec4 = glm::ivec4;
using UVec2 = glm::uvec2;
using UVec3 = glm::uvec3;
using UVec4 = glm::uvec4;
using Mat2 = glm::mat2;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;
using Quat = glm::quat;

using String = std::string;
using StringView = std::string_view;
using Path = std::filesystem::path;

/// @brief Distinct type for resource archive paths (vs direct filesystem Path).
struct ResPath {
	Path path;
	ResPath() = default;
	explicit ResPath(const Path& p) : path(p) {}
	explicit ResPath(const String& p) : path(p) {}
	explicit ResPath(const char* p) : path(p) {}
	ResPath& operator=(const String& p) { path = p; return *this; }
	String string() const { return path.string(); }
	operator const Path&() const { return path; }
};

#define ERes(path) ResPath(path)

template <typename T, Size size>
using Array = std::array<T, size>;
template <typename T>
using Vector = std::vector<T>;
template <typename K, typename T>
using Map = std::map<K, T>;
template <typename K, typename T>
using HashMap = std::unordered_map<K, T>;
template <typename T>
using Set = std::set<T>;
template <typename T>
using HashSet = std::unordered_set<T>;
template <typename T>
using Stack = std::stack<T>;
template <typename T>
using List = std::list<T>;

template <typename T>
using Uptr = std::unique_ptr<T>;
template <typename T>
using Sptr = std::shared_ptr<T>;
template <typename T>
using Wptr = std::weak_ptr<T>;

template <typename T>
using Optional = std::optional<T>;
inline constexpr std::nullopt_t NullOpt = std::nullopt;

using Mutex = std::mutex;
using SharedMutex = std::shared_mutex;

template <typename T, typename E>
    requires std::is_enum_v<E>
class Result {
public:
    /**
     * @brief Construct a successful Result.
     * @param val The success value.
     */
    Result(T val) : m_data(std::move(val)) {}

    /**
     * @brief Construct a failed Result.
     * @param err The error value.
     */
    Result(E err) : m_data(err) {}

    /// @return true if the result holds a success value.
    EE_NODISCARD bool isOk() const { return std::holds_alternative<T>(m_data); }

    /// @return true if the result holds an error value.
    EE_NODISCARD bool isErr() const { return std::holds_alternative<E>(m_data); }

    /// @return true if the result holds a success value (explicit bool conversion).
    EE_NODISCARD explicit operator bool() const { return isOk(); }

    /// @return The success value (undefined behavior if isErr()).
    EE_NODISCARD T& value() { return std::get<T>(m_data); }

    /// @return The success value (const overload).
    EE_NODISCARD const T& value() const { return std::get<T>(m_data); }

    /// @return The error value (undefined behavior if isOk()).
    EE_NODISCARD E& error() { return std::get<E>(m_data); }

    /// @return The error value (const overload).
    EE_NODISCARD const E& error() const { return std::get<E>(m_data); }

    /**
     * @brief Access the success value via arrow operator.
     * @return Pointer to the success value.
     */
    EE_NODISCARD T* operator->() { return &std::get<T>(m_data); }

    /**
     * @brief Access the success value via arrow operator (const).
     * @return Pointer to the success value.
     */
    EE_NODISCARD const T* operator->() const { return &std::get<T>(m_data); }

private:
    std::variant<T, E> m_data;  ///< Internal storage for either T or E.
};

/**
 * @brief Result<void, E> specialization for operations with no return value.
 * @tparam E The error value type (must be an enum class).
 */
template <typename E>
class Result<void, E> {
public:
    /// @brief Construct a successful void Result.
    Result() : m_ok(true) {}

    /// @brief Construct a failed void Result.
    Result(E err) : m_ok(false), m_error(err) {}

    /// @return true if the result indicates success.
    EE_NODISCARD bool isOk() const { return m_ok; }

    /// @return true if the result indicates failure.
    EE_NODISCARD bool isErr() const { return !m_ok; }

    /// @return true if the result indicates success (explicit bool conversion).
    EE_NODISCARD explicit operator bool() const { return m_ok; }

    /// @return The error value (undefined behavior if isOk()).
    EE_NODISCARD E& error() { return m_error; }

    /// @return The error value (const overload).
    EE_NODISCARD const E& error() const { return m_error; }

private:
    bool m_ok = true;     ///< Whether the result is successful.
    E    m_error{};       ///< Error value (only valid if m_ok is false).
};

/// @brief Transform holds position, rotation, and scale in 3D space.
///        Provides direction vectors, local/world rotation helpers, and matrix computation.
struct EE_API Transform {
    Vec3 position = Vec3(0.0f);   ///< Local position in world units.
    Quat rotation = Quat(1.0f, 0.0f, 0.0f, 0.0f);  ///< Local rotation as quaternion (w,x,y,z).
    Vec3 scale = Vec3(1.0f);   ///< Local scale per axis.

    // ================================================================
    // Direction vectors (world-space orientation of local axes)
    // ================================================================

    /// @brief Get the forward direction vector in world space
    EE_NODISCARD Vec3 forward() const { return rotation * Vec3(0, 0, -1); }
    /// @brief Get the up direction vector in world space
    EE_NODISCARD Vec3 up()      const { return rotation * Vec3(0, 1, 0); }
    /// @brief Get the right direction vector in world space
    EE_NODISCARD Vec3 right()   const { return rotation * Vec3(1, 0, 0); }

    // ================================================================
    // Transform modification
    // ================================================================

    /// Translate by a world-space delta.
    void translate(const Vec3& delta) { position += delta; }

    /// Rotate in local space (around own axes).
    void rotateLocal(const Vec3& eulerRadians) {
        rotation = rotation * Quat(eulerRadians);
    }

    /// Rotate in local space around a given axis by angle radians.
    void rotateLocalAxis(const Vec3& axis, F32 angle) {
        rotation = rotation * glm::angleAxis(angle, axis);
    }

    /// \deprecated Use rotateLocalAxis(axis, angle) — takes angle first, then axis.
    void rotateLocally(F32 angle, const Vec3& axis) { rotateLocalAxis(axis, angle); }

    /// Rotate in world space (around world axes).
    void rotateGlobal(const Vec3& eulerRadians) {
        rotation = Quat(eulerRadians) * rotation;
    }

    /// Rotate in world space around a given axis by angle radians.
    void rotateGlobalAxis(const Vec3& axis, F32 angle) {
        rotation = glm::angleAxis(angle, axis) * rotation;
    }

    /// Look at a target point from the current position.
    void lookAt(const Vec3& target, const Vec3& worldUp = Vec3(0, 1, 0)) {
        Mat4 view = glm::lookAt(position, target, worldUp);
        // Extract rotation from inverse view (view is world→camera, we want camera→world)
        rotation = glm::quat_cast(glm::inverse(Mat3(view)));
    }

    // ================================================================
    // Matrix computation
    // ================================================================

    /**
     * @brief Compute the world matrix T * R * S (Translation * Rotation * Scale).
     *        Column-major glm::mat4 ready for GPU upload.
     */
    EE_NODISCARD Mat4 computeWorldMatrix() const {
        Mat4 T = glm::translate(Mat4(1.0f), position);
        Mat4 R = glm::mat4_cast(rotation);
        Mat4 S = glm::scale(Mat4(1.0f), scale);
        return T * R * S;
    }

    /**
     * @brief Compute the normal matrix = transpose(inverse(T * R * S)).
     */
    EE_NODISCARD Mat4 computeNormalMatrix() const {
        Mat4 T = glm::translate(Mat4(1.0f), position);
        Mat4 R = glm::mat4_cast(rotation);
        Mat4 S = glm::scale(Mat4(1.0f), scale);
        return glm::transpose(glm::inverse(T * R * S));
    }

    // ================================================================
    // Static helpers
    // ================================================================

    /**
     * @brief Decompose a 4x4 TRS world matrix into position, rotation, and scale.
     */
    static Transform fromMatrix(const Mat4& world) {
        Transform t;
        t.position = Vec3(world[3]);
        t.scale    = Vec3(glm::length(Vec3(world[0])),
                          glm::length(Vec3(world[1])),
                          glm::length(Vec3(world[2])));
        if (t.scale.x > 0.0001f && t.scale.y > 0.0001f && t.scale.z > 0.0001f) {
            Mat3 rotMat(Vec3(world[0]) / t.scale.x,
                        Vec3(world[1]) / t.scale.y,
                        Vec3(world[2]) / t.scale.z);
            t.rotation = glm::quat_cast(rotMat);
        }
        return t;
    }

    /// \deprecated Use fromMatrix — lower-case to match naming conventions.
    static Transform FromMatrix(const Mat4& world) { return fromMatrix(world); }
};

/// @brief Align a value up to the nearest multiple of alignment.
template <typename T>
EE_NODISCARD constexpr T AlignUp(T value, T alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

/// @brief Clamp a value to the range [minVal, maxVal].
template <typename T>
EE_NODISCARD constexpr T Clamp(T value, T minVal, T maxVal) {
    return value < minVal ? minVal : (value > maxVal ? maxVal : value);
}

/// @brief Return the larger of two values.
template <typename T>
EE_NODISCARD constexpr T Max(T a, T b) {
    return a > b ? a : b;
}

/// @brief Return the smaller of two values.
template <typename T>
EE_NODISCARD constexpr T Min(T a, T b) {
    return a < b ? a : b;
}

/// @brief Apply an action to a target and set a changed flag if the value was modified.
template <typename T>
constexpr void checkIfChanged(T& target, bool& changed, std::function<void(T&)> action) {
    T oldv = target;
    action(target);
    if (oldv != target) {
        if (!changed) {
            changed = true;
        }
    }
}

EE_NAMESPACE_END