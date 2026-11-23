// test_storage.cpp — тесты хранилища (storage.cpp) и SHA-256.
#include "sha256.h"
#include "../src/storage.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <string>

namespace {

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

class StorageTest : public ::testing::Test {
protected:
    std::string usersPath_;
    std::string savesPath_;

    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        auto base = std::filesystem::temp_directory_path() /
                    (std::string(info->test_suite_name()) + "_" + info->name());
        usersPath_ = base.string() + "_users.txt";
        savesPath_ = base.string() + "_saves.txt";
        std::filesystem::remove(usersPath_);
        std::filesystem::remove(savesPath_);
    }

    void TearDown() override {
        std::filesystem::remove(usersPath_);
        std::filesystem::remove(savesPath_);
    }
};

}  // namespace

// ---------- SHA-256 ----------

TEST(Sha256, KnownVectors) {
    EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 56 байт — проверка дополнения на границе блока
    EXPECT_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

// ---------- Регистрация и вход ----------

TEST_F(StorageTest, RegisterNewUser) {
    Storage s(usersPath_, savesPath_);
    EXPECT_TRUE(s.registerUser("alice", "secret"));
    UserStats st;
    ASSERT_TRUE(s.getStats("alice", st));
    EXPECT_EQ(st.elo, 1200);
    EXPECT_EQ(st.wins + st.losses + st.draws, 0);
}

TEST_F(StorageTest, RegisterDuplicateFails) {
    Storage s(usersPath_, savesPath_);
    EXPECT_TRUE(s.registerUser("alice", "secret"));
    EXPECT_FALSE(s.registerUser("alice", "other"));
}

TEST_F(StorageTest, RegisterRejectsBadInput) {
    Storage s(usersPath_, savesPath_);
    EXPECT_FALSE(s.registerUser("bad name", "pass"));
    EXPECT_FALSE(s.registerUser("bob", ""));
    EXPECT_FALSE(s.registerUser("", "pass"));
}

TEST_F(StorageTest, LoginCorrectAndWrongPassword) {
    Storage s(usersPath_, savesPath_);
    s.registerUser("alice", "secret");
    EXPECT_TRUE(s.checkLogin("alice", "secret"));
    EXPECT_FALSE(s.checkLogin("alice", "Secret"));
    EXPECT_FALSE(s.checkLogin("nobody", "secret"));
}

TEST_F(StorageTest, FileStoresHashNotPassword) {
    {
        Storage s(usersPath_, savesPath_);
        s.registerUser("alice", "secret");
    }
    std::string content = readFile(usersPath_);
    EXPECT_EQ(content.find("secret"), std::string::npos);
    EXPECT_NE(content.find(sha256_hex("secret")), std::string::npos);
    EXPECT_EQ(content, "alice " + sha256_hex("secret") + " 0 0 0 1200\n");
}

TEST_F(StorageTest, DataSurvivesReload) {
    {
        Storage s(usersPath_, savesPath_);
        s.registerUser("alice", "a");
        s.registerUser("bob", "b");
        s.recordResult("alice", "bob", 1.0);
    }
    Storage s(usersPath_, savesPath_);
    EXPECT_TRUE(s.checkLogin("bob", "b"));
    UserStats a;
    ASSERT_TRUE(s.getStats("alice", a));
    EXPECT_EQ(a.wins, 1);
    EXPECT_EQ(a.elo, 1216);
}

// ---------- Elo ----------

TEST(Elo, Formula) {
    EXPECT_DOUBLE_EQ(elo_expected(1200, 1200), 0.5);
    EXPECT_NEAR(elo_expected(1400, 1200), 0.7597, 1e-4);
    EXPECT_EQ(elo_updated(1200, 1200, 1.0), 1216);
    EXPECT_EQ(elo_updated(1200, 1200, 0.0), 1184);
    EXPECT_EQ(elo_updated(1200, 1200, 0.5), 1200);
    EXPECT_EQ(elo_updated(1400, 1200, 1.0), 1408);  // 1400 + 32*(1-0.76) = 1407.7
}

TEST_F(StorageTest, RecordWinUpdatesStatsAndElo) {
    Storage s(usersPath_, savesPath_);
    s.registerUser("alice", "a");
    s.registerUser("bob", "b");
    s.recordResult("alice", "bob", 1.0);
    UserStats a, b;
    s.getStats("alice", a);
    s.getStats("bob", b);
    EXPECT_EQ(a.wins, 1);
    EXPECT_EQ(b.losses, 1);
    EXPECT_EQ(a.elo, 1216);
    EXPECT_EQ(b.elo, 1184);
}

TEST_F(StorageTest, RecordDrawBetweenUnequalPlayers) {
    Storage s(usersPath_, savesPath_);
    s.registerUser("alice", "a");
    s.registerUser("bob", "b");
    s.recordResult("alice", "bob", 1.0);  // 1216 / 1184
    s.recordResult("alice", "bob", 0.5);  // сильнейший теряет, слабейший получает
    UserStats a, b;
    s.getStats("alice", a);
    s.getStats("bob", b);
    EXPECT_EQ(a.draws, 1);
    EXPECT_EQ(b.draws, 1);
    EXPECT_LT(a.elo, 1216);
    EXPECT_GT(b.elo, 1184);
    EXPECT_EQ(a.elo + b.elo, 2400);
}

// ---------- Лидерборд ----------

TEST_F(StorageTest, LeaderboardSortedByEloTop5) {
    Storage s(usersPath_, savesPath_);
    for (const char* u : {"a", "b", "c", "d", "e", "f"}) s.registerUser(u, "p");
    s.recordResult("c", "a", 1.0);
    s.recordResult("c", "b", 1.0);
    s.recordResult("d", "e", 1.0);

    auto top = s.leaderboard(5);
    ASSERT_EQ(top.size(), 5u);
    EXPECT_EQ(top[0].name, "c");
    EXPECT_EQ(top[1].name, "d");
    for (size_t i = 1; i < top.size(); ++i) EXPECT_GE(top[i - 1].stats.elo, top[i].stats.elo);
    // f (1200) должен быть выше проигравших (<1200)
    EXPECT_EQ(top[2].name, "f");
}

TEST_F(StorageTest, LeaderboardTieBrokenByWinsThenName) {
    Storage s(usersPath_, savesPath_);
    s.registerUser("zed", "p");
    s.registerUser("amy", "p");
    auto top = s.leaderboard(5);
    ASSERT_EQ(top.size(), 2u);
    EXPECT_EQ(top[0].name, "amy");  // одинаковый Elo и победы — по алфавиту
}

// ---------- Сохранённые партии ----------

TEST_F(StorageTest, SaveFindAndErase) {
    {
        Storage s(usersPath_, savesPath_);
        s.saveGame(SavedGame{"alice", "bob", 'O', 3, 3, "X________"});
    }
    EXPECT_EQ(readFile(savesPath_), "alice bob O 3 3 X________\n");

    Storage s(usersPath_, savesPath_);  // загрузка с диска
    SavedGame g;
    ASSERT_TRUE(s.findSave("bob", g));
    EXPECT_EQ(g.p1, "alice");
    EXPECT_EQ(g.turn, 'O');
    EXPECT_EQ(g.board, "X________");

    s.eraseSavesWith("alice");
    EXPECT_FALSE(s.findSave("bob", g));
}

TEST_F(StorageTest, NewSaveReplacesOldSaveOfPlayer) {
    Storage s(usersPath_, savesPath_);
    s.saveGame(SavedGame{"alice", "bob", 'X', 3, 3, "_________"});
    s.saveGame(SavedGame{"carol", "alice", 'O', 5, 4, "X" + std::string(24, '_')});
    SavedGame g;
    EXPECT_FALSE(s.findSave("bob", g));  // старая партия alice удалена
    ASSERT_TRUE(s.findSave("alice", g));
    EXPECT_EQ(g.p1, "carol");
    EXPECT_EQ(g.n, 5);
}
