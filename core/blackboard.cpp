#include "core/blackboard.h"

namespace graphflow::core {

bool Blackboard::has(const std::string& key) const {
    auto it = key_to_hash_.find(key);
    return it != key_to_hash_.end() && data_.find(it->second) != data_.end();
}

void Blackboard::remove(const std::string& key) {
    auto keyIt = key_to_hash_.find(key);
    if (keyIt != key_to_hash_.end()) {
        auto hash = keyIt->second;
        key_to_hash_.erase(keyIt);
        hash_to_key_.erase(hash);
        data_.erase(hash);
    }
}

void Blackboard::clear() {
    data_.clear();
    key_to_hash_.clear();
    hash_to_key_.clear();
}

nlohmann::json Blackboard::toJson() const {
    nlohmann::json result;

    for (const auto& [hash, any] : data_) {
        auto keyIt = hash_to_key_.find(hash);
        if (keyIt == hash_to_key_.end()) {
            continue;
        }
        const std::string& key = keyIt->second;

        bool converted = false;

        if (auto ptr = std::any_cast<const int>(&any)) {
            result[key] = *ptr;
            converted = true;
        } else if (auto ptr = std::any_cast<const double>(&any)) {
            result[key] = *ptr;
            converted = true;
        } else if (auto ptr = std::any_cast<const float>(&any)) {
            result[key] = *ptr;
            converted = true;
        } else if (auto ptr = std::any_cast<const bool>(&any)) {
            result[key] = *ptr;
            converted = true;
        } else if (auto ptr = std::any_cast<const std::string>(&any)) {
            result[key] = *ptr;
            converted = true;
        } else if (auto ptr = std::any_cast<const long long>(&any)) {
            result[key] = *ptr;
            converted = true;
        } else if (auto ptr = std::any_cast<const unsigned int>(&any)) {
            result[key] = *ptr;
            converted = true;
        } else if (auto ptr = std::any_cast<const unsigned long long>(&any)) {
            result[key] = *ptr;
            converted = true;
        }

        if (!converted) {
            result[key] = "<unsupported type>";
        }
    }

    return result;
}

void Blackboard::fromJson(const nlohmann::json& json) {
    for (auto it = json.begin(); it != json.end(); ++it) {
        const std::string& key = it.key();
        const auto& value = it.value();

        auto hash = hash_string(key);

        if (value.is_boolean()) {
            data_[hash] = std::any(value.get<bool>());
        } else if (value.is_number_integer()) {
            auto num = value.get<long long>();
            if (num >= std::numeric_limits<int>::min() &&
                num <= std::numeric_limits<int>::max()) {
                data_[hash] = std::any(static_cast<int>(num));
            } else {
                data_[hash] = std::any(num);
            }
        } else if (value.is_number_unsigned()) {
            auto num = value.get<unsigned long long>();
            if (num <= std::numeric_limits<unsigned int>::max()) {
                data_[hash] = std::any(static_cast<unsigned int>(num));
            } else {
                data_[hash] = std::any(num);
            }
        } else if (value.is_number_float()) {
            data_[hash] = std::any(value.get<double>());
        } else if (value.is_string()) {
            data_[hash] = std::any(value.get<std::string>());
        }

        key_to_hash_[key] = hash;
        hash_to_key_[hash] = key;
    }
}

std::vector<std::string> Blackboard::getKeys() const {
    std::vector<std::string> keys;
    keys.reserve(key_to_hash_.size());

    for (const auto& [key, _] : key_to_hash_) {
        keys.push_back(key);
    }
    return keys;
}

}  // namespace graphflow::core
