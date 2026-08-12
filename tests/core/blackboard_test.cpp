// tests/core/blackboard_test.cpp
// Blackboard type-safe / string-key / json 往返测试。完整用例由子代理补全。
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include "core/blackboard.h"

using graphflow::core::Blackboard;

// 测试用自定义结构
struct Vec3 {
    int x;
    int y;
    int z;

    bool operator==(const Vec3& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};

// 测试用 movable-and-copyable 类型（std::any 要求 CopyConstructible，
// 故不可拷贝类型无法存入 Blackboard，此处验证可拷贝 move 语义类型）。
struct Movable {
    int data;
    explicit Movable(int v) : data(v) {}
    Movable(const Movable&) = default;
    Movable& operator=(const Movable&) = default;
    Movable(Movable&&) noexcept = default;
    Movable& operator=(Movable&&) noexcept = default;
    int value() const { return data; }
};

// ============================================================================
// 1. type-safe emplace/get 基本类型
// ============================================================================

TEST(BlackboardTest, EmplaceGetInt) {
    Blackboard bb;
    bb.emplace<int>(42);
    EXPECT_EQ(bb.get<int>(), 42);
}

TEST(BlackboardTest, EmplaceGetString) {
    Blackboard bb;
    bb.emplace<std::string>("hello");
    EXPECT_EQ(bb.get<std::string>(), "hello");
}

TEST(BlackboardTest, EmplaceGetCustomStruct) {
    Blackboard bb;
    bb.emplace<Vec3>(Vec3{1, 2, 3});
    EXPECT_EQ(bb.get<Vec3>(), (Vec3{1, 2, 3}));
}

TEST(BlackboardTest, EmplaceWithArgsConstruction) {
    Blackboard bb;
    // emplace 用参数直接构造，避免临时对象
    bb.emplace<std::string>(4, 'x');
    EXPECT_EQ(bb.get<std::string>(), "xxxx");
}

TEST(BlackboardTest, EmplaceAsWithExplicitId) {
    Blackboard bb;
    Blackboard::id_type id = 100;
    bb.emplace_as<std::string>(id, "explicit");
    EXPECT_EQ(bb.get_as<std::string>(id), "explicit");
}

// ============================================================================
// 2. find 返回 nullptr（不存在）/ 非 nullptr（存在）
// ============================================================================

TEST(BlackboardTest, FindReturnsNullptrWhenAbsent) {
    Blackboard bb;
    EXPECT_EQ(bb.find<int>(), nullptr);
}

TEST(BlackboardTest, FindReturnsPtrWhenPresent) {
    Blackboard bb;
    bb.emplace<int>(7);
    auto* ptr = bb.find<int>();
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(*ptr, 7);
}

TEST(BlackboardTest, FindConstReturnsPtrWhenPresent) {
    Blackboard bb;
    bb.emplace<std::string>("abc");
    const Blackboard& cbb = bb;
    auto* ptr = cbb.find<std::string>();
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(*ptr, "abc");
}

TEST(BlackboardTest, FindAsReturnsNullptrWhenAbsent) {
    Blackboard bb;
    EXPECT_EQ(bb.find_as<int>(999), nullptr);
}

TEST(BlackboardTest, FindAsReturnsPtrWhenPresent) {
    Blackboard bb;
    Blackboard::id_type id = 55;
    bb.emplace_as<double>(id, 3.14);
    auto* ptr = bb.find_as<double>(id);
    ASSERT_NE(ptr, nullptr);
    EXPECT_DOUBLE_EQ(*ptr, 3.14);
}

// ============================================================================
// 3. contains / erase 返回值
// ============================================================================

TEST(BlackboardTest, ContainsReturnsFalseWhenAbsent) {
    Blackboard bb;
    EXPECT_FALSE(bb.contains<int>());
}

TEST(BlackboardTest, ContainsReturnsTrueWhenPresent) {
    Blackboard bb;
    bb.emplace<int>(1);
    EXPECT_TRUE(bb.contains<int>());
}

TEST(BlackboardTest, ContainsAsReturnsFalseWhenAbsent) {
    Blackboard bb;
    EXPECT_FALSE(bb.contains_as<int>(1));
}

TEST(BlackboardTest, ContainsAsReturnsTrueWhenPresent) {
    Blackboard bb;
    Blackboard::id_type id = 7;
    bb.emplace_as<int>(id, 42);
    EXPECT_TRUE(bb.contains_as<int>(id));
}

TEST(BlackboardTest, EraseReturnsTrueWhenPresent) {
    Blackboard bb;
    bb.emplace<int>(10);
    EXPECT_TRUE(bb.erase<int>());
    EXPECT_FALSE(bb.contains<int>());
}

TEST(BlackboardTest, EraseReturnsFalseWhenAbsent) {
    Blackboard bb;
    EXPECT_FALSE(bb.erase<int>());
}

TEST(BlackboardTest, EraseAsReturnsTrueWhenPresent) {
    Blackboard bb;
    Blackboard::id_type id = 3;
    bb.emplace_as<int>(id, 99);
    EXPECT_TRUE(bb.erase_as<int>(id));
    EXPECT_FALSE(bb.contains_as<int>(id));
}

TEST(BlackboardTest, EraseAsReturnsFalseWhenAbsent) {
    Blackboard bb;
    EXPECT_FALSE(bb.erase_as<int>(404));
}

TEST(BlackboardTest, EraseDoesNotAffectOtherTypes) {
    Blackboard bb;
    bb.emplace<int>(1);
    bb.emplace<std::string>("two");
    EXPECT_TRUE(bb.erase<int>());
    EXPECT_FALSE(bb.contains<int>());
    EXPECT_TRUE(bb.contains<std::string>());
}

// ============================================================================
// 4. get_or 默认值返回（不存在时）
// ============================================================================

TEST(BlackboardTest, GetOrReturnsValueWhenPresent) {
    Blackboard bb;
    bb.emplace<int>(123);
    EXPECT_EQ(bb.get_or<int>(), 123);
}

TEST(BlackboardTest, GetOrReturnsDefaultWhenAbsent) {
    Blackboard bb;
    EXPECT_EQ(bb.get_or<int>(42), 42);
}

TEST(BlackboardTest, GetOrReturnsDefaultConstructedWhenAbsent) {
    Blackboard bb;
    // 不传默认值，使用 Type{} = 0
    EXPECT_EQ(bb.get_or<int>(), 0);
}

TEST(BlackboardTest, GetOrAsReturnsValueWhenPresent) {
    Blackboard bb;
    Blackboard::id_type id = 8;
    bb.emplace_as<int>(id, 77);
    EXPECT_EQ(bb.get_or<int>(id, -1), 77);
}

TEST(BlackboardTest, GetOrAsReturnsDefaultWhenAbsent) {
    Blackboard bb;
    EXPECT_EQ(bb.get_or<int>(404, -1), -1);
}

// ============================================================================
// 5. insert_or_assign 覆盖已有值
// ============================================================================

TEST(BlackboardTest, InsertOrAssignOverwritesExisting) {
    Blackboard bb;
    bb.emplace<int>(1);
    EXPECT_EQ(bb.get<int>(), 1);
    bb.insert_or_assign<int>(2);
    EXPECT_EQ(bb.get<int>(), 2);
}

TEST(BlackboardTest, InsertOrAssignInsertsWhenAbsent) {
    Blackboard bb;
    EXPECT_FALSE(bb.contains<int>());
    bb.insert_or_assign<int>(5);
    EXPECT_TRUE(bb.contains<int>());
    EXPECT_EQ(bb.get<int>(), 5);
}

TEST(BlackboardTest, InsertOrAssignAsOverwritesExisting) {
    Blackboard bb;
    Blackboard::id_type id = 9;
    bb.emplace_as<std::string>(id, "old");
    bb.insert_or_assign_as<std::string>(id, std::string("new"));
    EXPECT_EQ(bb.get_as<std::string>(id), "new");
}

TEST(BlackboardTest, InsertOrAssignReturnsRefToStored) {
    Blackboard bb;
    auto& ref = bb.insert_or_assign<int>(100);
    EXPECT_EQ(ref, 100);
    ref = 200;
    EXPECT_EQ(bb.get<int>(), 200);
}

// ============================================================================
// 6. string-key set/get/has/remove/getKeys/clear
// ============================================================================

TEST(BlackboardTest, StringKeySetGetBasic) {
    Blackboard bb;
    bb.set<int>("count", 42);
    EXPECT_EQ(bb.get<int>("count"), 42);
}

TEST(BlackboardTest, StringKeySetGetString) {
    Blackboard bb;
    bb.set<std::string>("name", std::string("graphflow"));
    EXPECT_EQ(bb.get<std::string>("name"), "graphflow");
}

TEST(BlackboardTest, StringKeySetOverwrite) {
    Blackboard bb;
    bb.set<int>("v", 1);
    bb.set<int>("v", 2);
    EXPECT_EQ(bb.get<int>("v"), 2);
}

TEST(BlackboardTest, StringKeyGetReturnsDefaultWhenAbsent) {
    Blackboard bb;
    EXPECT_EQ(bb.get<int>("missing", -1), -1);
}

TEST(BlackboardTest, StringKeyHasReturnsTrueWhenPresent) {
    Blackboard bb;
    bb.set<int>("k", 1);
    EXPECT_TRUE(bb.has("k"));
}

TEST(BlackboardTest, StringKeyHasReturnsFalseWhenAbsent) {
    Blackboard bb;
    EXPECT_FALSE(bb.has("nope"));
}

TEST(BlackboardTest, StringKeyRemoveErasesEntry) {
    Blackboard bb;
    bb.set<int>("k", 1);
    EXPECT_TRUE(bb.has("k"));
    bb.remove("k");
    EXPECT_FALSE(bb.has("k"));
}

TEST(BlackboardTest, StringKeyRemoveNoOpWhenAbsent) {
    Blackboard bb;
    bb.remove("never-set");
    EXPECT_FALSE(bb.has("never-set"));
}

TEST(BlackboardTest, StringKeyGetKeysReturnsAllSetKeys) {
    Blackboard bb;
    bb.set<int>("a", 1);
    bb.set<std::string>("b", std::string("two"));
    bb.set<double>("c", 3.0);
    auto keys = bb.getKeys();
    ASSERT_EQ(keys.size(), 3u);
    // getKeys 顺序未保证，做存在性检查
    int found = 0;
    for (const auto& k : keys) {
        if (k == "a" || k == "b" || k == "c") ++found;
    }
    EXPECT_EQ(found, 3);
}

TEST(BlackboardTest, StringKeyClearRemovesAllStringEntries) {
    Blackboard bb;
    bb.set<int>("a", 1);
    bb.set<int>("b", 2);
    bb.clear();
    EXPECT_FALSE(bb.has("a"));
    EXPECT_FALSE(bb.has("b"));
    EXPECT_EQ(bb.size(), 0u);
}

TEST(BlackboardTest, StringKeySizeReflectsEntryCount) {
    Blackboard bb;
    EXPECT_EQ(bb.size(), 0u);
    bb.set<int>("a", 1);
    EXPECT_EQ(bb.size(), 1u);
    bb.set<int>("b", 2);
    EXPECT_EQ(bb.size(), 2u);
}

TEST(BlackboardTest, StringKeyEmptyReflectsState) {
    Blackboard bb;
    EXPECT_TRUE(bb.empty());
    bb.set<int>("a", 1);
    EXPECT_FALSE(bb.empty());
}

// ============================================================================
// 7. toJson + fromJson 往返（int/double/bool/string）
// ============================================================================

TEST(BlackboardTest, JsonRoundTripInt) {
    Blackboard bb;
    bb.set<int>("count", 42);
    auto j = bb.toJson();
    EXPECT_EQ(j["count"].get<int>(), 42);

    Blackboard dst;
    dst.fromJson(j);
    EXPECT_EQ(dst.get<int>("count"), 42);
}

TEST(BlackboardTest, JsonRoundTripDouble) {
    Blackboard bb;
    bb.set<double>("pi", 3.14);
    auto j = bb.toJson();
    EXPECT_DOUBLE_EQ(j["pi"].get<double>(), 3.14);

    Blackboard dst;
    dst.fromJson(j);
    EXPECT_DOUBLE_EQ(dst.get<double>("pi"), 3.14);
}

TEST(BlackboardTest, JsonRoundTripBool) {
    Blackboard bb;
    bb.set<bool>("flag", true);
    auto j = bb.toJson();
    EXPECT_TRUE(j["flag"].get<bool>());

    Blackboard dst;
    dst.fromJson(j);
    EXPECT_TRUE(dst.get<bool>("flag"));
}

TEST(BlackboardTest, JsonRoundTripString) {
    Blackboard bb;
    bb.set<std::string>("name", std::string("graphflow"));
    auto j = bb.toJson();
    EXPECT_EQ(j["name"].get<std::string>(), "graphflow");

    Blackboard dst;
    dst.fromJson(j);
    EXPECT_EQ(dst.get<std::string>("name"), "graphflow");
}

TEST(BlackboardTest, JsonRoundTripMultipleTypes) {
    Blackboard bb;
    bb.set<int>("i", 1);
    bb.set<double>("d", 2.5);
    bb.set<bool>("b", true);
    bb.set<std::string>("s", std::string("four"));

    auto j = bb.toJson();
    Blackboard dst;
    dst.fromJson(j);

    EXPECT_EQ(dst.get<int>("i"), 1);
    EXPECT_DOUBLE_EQ(dst.get<double>("d"), 2.5);
    EXPECT_TRUE(dst.get<bool>("b"));
    EXPECT_EQ(dst.get<std::string>("s"), "four");
}

TEST(BlackboardTest, JsonEmptyBlackboardProducesNull) {
    // 空Blackboard：toJson 内部 nlohmann::json result; 默认初始化为 null，
    // 无 string-key 条目时不会被 [] 写成对象。反映产品实现：空表 toJson 返回 null。
    Blackboard bb;
    auto j = bb.toJson();
    EXPECT_TRUE(j.is_null());
}

TEST(BlackboardTest, JsonFromEmptyJsonLeavesBlackboardEmpty) {
    Blackboard bb;
    nlohmann::json empty = nlohmann::json::object();
    bb.fromJson(empty);
    EXPECT_EQ(bb.size(), 0u);
}

// ============================================================================
// 8. 类型不匹配时 get 返回默认或 find 返回 nullptr
// ============================================================================

TEST(BlackboardTest, TypeMismatchFindReturnsNullptr) {
    Blackboard bb;
    bb.emplace<int>(42);
    // 存 int，以 double 查 find → nullptr
    EXPECT_EQ(bb.find<double>(), nullptr);
}

TEST(BlackboardTest, TypeMismatchContainsReturnsFalse) {
    Blackboard bb;
    bb.emplace<int>(42);
    // contains 检查 id 存在且类型匹配
    EXPECT_FALSE(bb.contains<double>());
    EXPECT_TRUE(bb.contains<int>());
}

TEST(BlackboardTest, TypeMismatchGetOrReturnsDefault) {
    Blackboard bb;
    bb.emplace<int>(42);
    // get_or<double> 找不到 double → 默认
    EXPECT_EQ(bb.get_or<double>(-1.0), -1.0);
}

TEST(BlackboardTest, TypeMismatchEraseReturnsFalse) {
    Blackboard bb;
    bb.emplace<int>(42);
    // erase<double>：id 存在但类型不匹配 → false
    EXPECT_FALSE(bb.erase<double>());
    // 原 int 仍在
    EXPECT_TRUE(bb.contains<int>());
}

// ============================================================================
// 9. move 语义（Blackboard&& 可构造赋值）
// ============================================================================

TEST(BlackboardTest, MoveConstructorTransfersOwnership) {
    Blackboard src;
    src.emplace<int>(10);
    src.emplace<std::string>("moved");
    size_t srcSize = src.size();

    Blackboard dst(std::move(src));
    EXPECT_EQ(dst.size(), srcSize);
    EXPECT_EQ(dst.get<int>(), 10);
    EXPECT_EQ(dst.get<std::string>(), "moved");
}

TEST(BlackboardTest, MoveAssignmentTransfersOwnership) {
    Blackboard src;
    src.emplace<int>(99);

    Blackboard dst;
    dst.emplace<int>(1);  // dst 原有数据
    dst = std::move(src);

    EXPECT_EQ(dst.get<int>(), 99);
}

TEST(BlackboardTest, MoveLeavesSourceEmpty) {
    Blackboard src;
    src.emplace<int>(5);
    size_t expectedSize = src.size();

    Blackboard dst(std::move(src));
    // 源对象移动后应为空（unordered_map move 后 empty）
    EXPECT_EQ(src.size(), 0u);
    EXPECT_EQ(dst.size(), expectedSize);
}

TEST(BlackboardTest, MovableTypeStored) {
    // std::any 要求 CopyConstructible，故 Blackboard 只能存可拷贝类型。
    // 此处验证 move 构造仍可用于 set/emplace 语义。
    Blackboard bb;
    bb.emplace<Movable>(Movable(7));
    EXPECT_EQ(bb.get<Movable>().value(), 7);
}
