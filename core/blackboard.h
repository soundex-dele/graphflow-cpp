#pragma once

#include <any>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace graphflow::core {

/**
 * @brief Type-safe context storage
 *
 * Stores values keyed by type hash (type-safe) or string keys (legacy API).
 * NOT thread-safe - user must handle synchronization if needed.
 *
 * BT 层用法(bt/ScopedBlackboard):对本作用域 m_local 加 mutex 保护单 tick 内
 *   同步访问 + 异步 Action 跨 tick 写;但跨作用域并发访问需调用方协调。
 *   非 BT 用户直接用本类时,同步主线程串行访问安全,并发需自加锁。
 */
class Blackboard {
public:
    using id_type = size_t;

    Blackboard() = default;
    ~Blackboard() = default;

    // Disable copy
    Blackboard(const Blackboard&) = delete;
    Blackboard& operator=(const Blackboard&) = delete;

    // Enable move
    Blackboard(Blackboard&&) noexcept = default;
    Blackboard& operator=(Blackboard&&) noexcept = default;

    // ========================================================================
    // Type-safe API
    // ========================================================================

    /** @brief Emplace a value with explicit id */
    template<typename Type, typename... Args>
    Type& emplace_as(const id_type id, Args&&... args) {
        data_[id] = std::any(Type(std::forward<Args>(args)...));
        return *std::any_cast<Type>(&data_[id]);
    }

    /** @brief Emplace a value using type hash as key */
    template<typename Type, typename... Args>
    Type& emplace(Args&&... args) {
        return emplace_as<Type>(type_hash<Type>(), std::forward<Args>(args)...);
    }

    /** @brief Insert or assign a value with explicit id */
    template<typename Type>
    Type& insert_or_assign_as(const id_type id, Type&& value) {
        using DecayType = std::remove_cv_t<std::remove_reference_t<Type>>;
        data_[id] = std::any(std::forward<Type>(value));
        return *std::any_cast<DecayType>(&data_[id]);
    }

    /** @brief Insert or assign a value using type hash as key */
    template<typename Type>
    Type& insert_or_assign(Type&& value) {
        return insert_or_assign_as(type_hash<Type>(), std::forward<Type>(value));
    }

    /** @brief Get a const value by id */
    template<typename Type>
    const Type& get_as(const id_type id) const {
        return *std::any_cast<const Type>(&data_.at(id));
    }

    /** @brief Get a value by id */
    template<typename Type>
    Type& get_as(const id_type id) {
        return *std::any_cast<Type>(&data_.at(id));
    }

    /** @brief Get a const value by type hash */
    template<typename Type>
    const Type& get() const {
        return get_as<Type>(type_hash<Type>());
    }

    /** @brief Get a value by type hash */
    template<typename Type>
    Type& get() {
        return get_as<Type>(type_hash<Type>());
    }

    /** @brief Find a const value by id, returns nullptr if not found */
    template<typename Type>
    const Type* find_as(const id_type id) const {
        auto it = data_.find(id);
        if (it != data_.end()) {
            return std::any_cast<const Type>(&it->second);
        }
        return nullptr;
    }

    /** @brief Find a value by id, returns nullptr if not found */
    template<typename Type>
    Type* find_as(const id_type id) {
        auto it = data_.find(id);
        if (it != data_.end()) {
            return std::any_cast<Type>(&it->second);
        }
        return nullptr;
    }

    /** @brief Find a const value by type hash */
    template<typename Type>
    const Type* find() const {
        return find_as<Type>(type_hash<Type>());
    }

    /** @brief Find a value by type hash */
    template<typename Type>
    Type* find() {
        return find_as<Type>(type_hash<Type>());
    }

    /** @brief Get a value or return default if not found */
    template<typename Type>
    Type get_or(const id_type id, const Type& defaultValue = Type{}) const {
        auto it = data_.find(id);
        if (it != data_.end()) {
            if (auto ptr = std::any_cast<const Type>(&it->second)) {
                return *ptr;
            }
        }
        return defaultValue;
    }

    /** @brief Get a value by type hash or return default */
    template<typename Type>
    Type get_or(const Type& defaultValue = Type{}) const {
        return get_or<Type>(type_hash<Type>(), defaultValue);
    }

    /** @brief Check if a value with given id and type exists */
    template<typename Type>
    bool contains_as(const id_type id) const {
        auto it = data_.find(id);
        return it != data_.end() && std::any_cast<const Type>(&it->second) != nullptr;
    }

    /** @brief Check if a value with given type exists */
    template<typename Type>
    bool contains() const {
        return contains_as<Type>(type_hash<Type>());
    }

    /** @brief Erase a value by id */
    template<typename Type>
    bool erase_as(const id_type id) {
        auto it = data_.find(id);
        if (it != data_.end() && std::any_cast<const Type>(&it->second) != nullptr) {
            data_.erase(it);
            return true;
        }
        return false;
    }

    /** @brief Erase a value by type hash */
    template<typename Type>
    bool erase() {
        return erase_as<Type>(type_hash<Type>());
    }

    // ========================================================================
    // String key API (Legacy - 向后兼容)
    // ========================================================================

    template<typename Type>
    void set(const std::string& key, const Type& value) {
        auto hash = hash_string(key);
        data_[hash] = std::any(value);
        key_to_hash_[key] = hash;
        hash_to_key_[hash] = key;
    }

    template<typename Type>
    Type get(const std::string& key, const Type& defaultValue = Type{}) const {
        auto hash = hash_string(key);
        auto it = data_.find(hash);
        if (it != data_.end()) {
            if (auto ptr = std::any_cast<const Type>(&it->second)) {
                return *ptr;
            }
        }
        return defaultValue;
    }

    bool has(const std::string& key) const;
    void remove(const std::string& key);
    void clear();
    std::vector<std::string> getKeys() const;
    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& json);

    size_t size() const { return data_.size(); }
    bool empty() const { return data_.empty(); }

private:
    /** FNV-1a hash for string keys */
    static id_type hash_string(std::string_view str) {
        id_type hash = 14695981039346656037ULL;
        for (char c : str) {
            hash ^= static_cast<id_type>(static_cast<unsigned char>(c));
            hash *= 1099511628211ULL;
        }
        return hash ^ 0x8000000000000000ULL;
    }

    /** Compile-time type hash using type name */
    template<typename Type>
    static constexpr id_type type_hash() noexcept {
        return std::type_index(typeid(Type)).hash_code();
    }

    std::unordered_map<id_type, std::any> data_;
    std::unordered_map<std::string, id_type> key_to_hash_;
    std::unordered_map<id_type, std::string> hash_to_key_;
};

}  // namespace graphflow::core
