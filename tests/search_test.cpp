// Search indexes (FT.*) and fields with several values.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <random>
#include <thread>

#include "cache/object_cache.h"
#include "db/database.h"
#include "index/index_def.h"
#include "index/search_query.h"
#include "server/redis_commands.h"
#include "storage/rocks_storage.h"

using namespace datadam;
namespace fs = std::filesystem;

namespace {

// A RESP2 reply: a bulk/simple/integer/error string, or an array of replies.
struct Value {
  std::string text;
  bool array = false, error = false, null = false;
  std::vector<Value> items;
};

Value parse(std::string_view& in) {
  Value v;
  char type = in[0];
  size_t eol = in.find("\r\n");
  std::string_view head = in.substr(1, eol - 1);
  in.remove_prefix(eol + 2);
  if (type == '*') {
    v.array = true;
    int n = std::stoi(std::string(head));
    if (n < 0) v.null = true;
    for (int i = 0; i < n; ++i) v.items.push_back(parse(in));
  } else if (type == '$') {
    int n = std::stoi(std::string(head));
    if (n < 0) {
      v.null = true;
    } else {
      v.text = std::string(in.substr(0, static_cast<size_t>(n)));
      in.remove_prefix(static_cast<size_t>(n) + 2);
    }
  } else {
    v.text = std::string(head);
    v.error = type == '-';
  }
  return v;
}

class Search : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("datadam_search_" + std::to_string(std::random_device{}()));
    StorageOptions so;
    so.dir = dir_.string();
    so.fsync = FsyncPolicy::No;
    storage_ = RocksStorage::open(so);
    CacheOptions co;
    co.max_memory = 4 << 20;
    co.min_memory = 0;
    co.shards = 2;
    cache_ = std::make_unique<ObjectCache>(co);
    db_ = std::make_unique<Database>(*storage_, *cache_);
    ctx_ = std::make_unique<ServerContext>(*db_, config_, stats_, 0);
  }
  void TearDown() override {
    ctx_.reset();
    db_.reset();
    cache_.reset();
    storage_.reset();
    fs::remove_all(dir_);
  }

  Value run(std::vector<std::string> args) {
    std::vector<std::string_view> views(args.begin(), args.end());
    std::string out;
    Reply reply(out, 2);
    execute_redis_command(*ctx_, client_, views, reply);
    std::string_view in = out;
    return parse(in);
  }
  std::string text(std::vector<std::string> args) { return run(std::move(args)).text; }
  std::vector<std::string> items(std::vector<std::string> args) {
    std::vector<std::string> out;
    for (const auto& i : run(std::move(args)).items) out.push_back(i.null ? "nil" : i.text);
    return out;
  }
  // FT.SEARCH ... NOCONTENT (added here): the count, then the keys.
  std::vector<std::string> keys(std::vector<std::string> args) {
    args.insert(args.begin(), "FT.SEARCH");
    args.push_back("NOCONTENT");
    return items(std::move(args));
  }
  // Waits until an index is no longer being built (FT.INFO or IDX.INFO).
  void ready(const std::string& name, bool ft = true) {
    for (int i = 0; i < 500; ++i) {
      auto info = items({ft ? "FT.INFO" : "IDX.INFO", name});
      for (size_t k = 0; k + 1 < info.size(); ++k) {
        if (info[k] == "state" && info[k + 1] == "ready") return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    FAIL() << "index " << name << " not ready";
  }

  fs::path dir_;
  Config config_;
  ServerStats stats_;
  ClientState client_;
  std::unique_ptr<RocksStorage> storage_;
  std::unique_ptr<ObjectCache> cache_;
  std::unique_ptr<Database> db_;
  std::unique_ptr<ServerContext> ctx_;
};

IndexDef search_def() {
  IndexDef def;
  def.id = 3;
  def.name = "s";
  def.kind = IndexKind::Search;
  def.fields = {{"tags", IndexFieldType::Tag, "", ',', false}, {"n", IndexFieldType::Numeric, "num"}};
  return def;
}

}  // namespace

TEST(SearchIndexDef, EncodesAndSplitsEntries) {
  IndexDef def = search_def();
  ASSERT_EQ(def.compile(), "");
  IndexDef back;
  ASSERT_TRUE(IndexDef::decode(def.encode(), &back));
  EXPECT_TRUE(back.search());
  EXPECT_EQ(back.fields[0].separator, ',');
  EXPECT_FALSE(back.fields[0].case_sensitive);
  EXPECT_EQ(back.hash_field(1), "num");
  // Fields are independent: a missing one leaves the others (and the presence entry).
  auto value_of = [](std::string_view f) -> std::optional<std::string> {
    if (f == "tags") return std::string(" Red, blue ,,RED");
    return std::nullopt;
  };
  IndexDef::Tuples t = back.tuples(value_of);
  ASSERT_EQ(t.size(), 3u);  // red, blue, presence
  for (const auto& tuple : t) {
    std::string key = back.entry_key(tuple, "k1");
    IndexDef::Entry e;
    ASSERT_TRUE(back.split_entry(key, back.entry_value(1, 0, "k1"), &e));
    EXPECT_EQ(e.owner, "k1");
    EXPECT_EQ(e.field, tuple[0] == IndexDef::kPresence ? -1 : 0);
  }
  std::string v;
  ASSERT_TRUE(decode_tag(t[0].substr(1), &v));
  EXPECT_EQ(v, "blue");
}

TEST(SearchIndexDef, CompoundTuplesAreCombinations) {
  IndexDef def;
  def.fields = {{"a", IndexFieldType::Tag, "", '|'}, {"b", IndexFieldType::Numeric, ""}};
  auto t = def.tuples([](std::string_view f) -> std::optional<std::string> {
    return f == "a" ? std::string("x|y|x") : std::string("5");
  });
  EXPECT_EQ(t.size(), 2u);
  // Every field is needed, and a NUMERIC must be a number.
  EXPECT_TRUE(def.tuples([](std::string_view f) -> std::optional<std::string> {
                   return f == "a" ? std::optional<std::string>("x") : std::nullopt;
                 }).empty());
  EXPECT_TRUE(def.tuples([](std::string_view f) -> std::optional<std::string> {
                   return f == "a" ? std::string("x") : std::string("five");
                 }).empty());
}

TEST(SearchQuery, ParsesTheRediSearchSyntax) {
  IndexDef def = search_def();
  std::string error;
  using Kind = SearchNode::Kind;
  auto q = parse_search_query("@tags:{Red | bl*} -@n:[(1 +inf] | *", def, {}, &error);
  ASSERT_TRUE(q) << error;
  // Union binds tighter than intersection.
  ASSERT_EQ(q->kind, Kind::And);
  ASSERT_EQ(q->children.size(), 2u);
  const SearchNode& tags = q->children[0];
  EXPECT_EQ(tags.kind, Kind::Tag);
  EXPECT_EQ(tags.values, (std::vector<std::string>{"red", "bl"}));
  EXPECT_EQ(tags.prefix, (std::vector<bool>{false, true}));
  ASSERT_EQ(q->children[1].kind, Kind::Or);
  const SearchNode& range = q->children[1].children[0].children[0];
  EXPECT_EQ(range.kind, Kind::Range);
  EXPECT_TRUE(range.lo_open);
  EXPECT_EQ(range.lo, 1);
  EXPECT_TRUE(std::isinf(range.hi));
  // Escapes, quotes and parameters.
  SearchParams params{{"t", "a b"}, {"min", "2"}};
  q = parse_search_query(R"(@tags:{x\-y | "q|r" | $t} @n:[$min 10])", def, params, &error);
  ASSERT_TRUE(q) << error;
  EXPECT_EQ(q->children[0].values, (std::vector<std::string>{"x-y", "q|r", "a b"}));
  EXPECT_EQ(q->children[1].lo, 2);
  for (const char* bad : {"hello", "@nope:{x}", "@n:{x}", "@tags:[1 2]", "@tags:{x", "@tags:{}", "(@n:[1 2]",
                          "@n:[1 x]", "@tags:{$missing}", "@n:[1 2])"}) {
    EXPECT_FALSE(parse_search_query(bad, def, {}, &error)) << bad;
  }
}

TEST_F(Search, HashTagsNumbersAndBooleanQueries) {
  ASSERT_EQ(text({"FT.CREATE", "items", "ON", "HASH", "PREFIX", "1", "item:", "SCHEMA", "color", "TAG", "price",
                  "NUMERIC", "SORTABLE", "size", "AS", "sz", "TAG", "CASESENSITIVE"}),
            "OK");
  EXPECT_EQ(text({"FT.CREATE", "items", "SCHEMA", "x", "TAG"}), "ERR Index already exists");
  EXPECT_TRUE(run({"FT.CREATE", "t", "SCHEMA", "body", "TEXT"}).error);
  text({"HSET", "item:1", "color", "Red, Green", "price", "10", "size", "M"});
  text({"HSET", "item:2", "color", "blue", "price", "25.5", "size", "m"});
  text({"HSET", "item:3", "color", "red", "price", "40"});
  text({"HSET", "item:4", "name", "no indexed fields"});
  text({"HSET", "other:1", "color", "red"});  // not under the prefix
  ready("items");

  EXPECT_EQ(keys({"items", "@color:{red}"}), (std::vector<std::string>{"2", "item:1", "item:3"}));
  EXPECT_EQ(keys({"items", "@color:{RED | blue}"}), (std::vector<std::string>{"3", "item:1", "item:2", "item:3"}));
  EXPECT_EQ(keys({"items", "@color:{gr*}"}), (std::vector<std::string>{"1", "item:1"}));
  EXPECT_EQ(keys({"items", "@price:[10 25.5]"}), (std::vector<std::string>{"2", "item:1", "item:2"}));
  EXPECT_EQ(keys({"items", "@price:[(10 (40]"}), (std::vector<std::string>{"1", "item:2"}));
  EXPECT_EQ(keys({"items", "@price:[-inf +inf]"}).front(), "3");
  EXPECT_EQ(keys({"items", "@color:{red} @price:[20 +inf]"}), (std::vector<std::string>{"1", "item:3"}));
  EXPECT_EQ(keys({"items", "@color:{red} -@price:[20 +inf]"}), (std::vector<std::string>{"1", "item:1"}));
  EXPECT_EQ(keys({"items", "-@color:{red}"}), (std::vector<std::string>{"2", "item:2", "item:4"}));
  EXPECT_EQ(keys({"items", "*"}), (std::vector<std::string>{"4", "item:1", "item:2", "item:3", "item:4"}));
  EXPECT_EQ(keys({"items", "@sz:{M}"}), (std::vector<std::string>{"1", "item:1"}));  // CASESENSITIVE
  EXPECT_EQ(keys({"items", "(@sz:{m} | @color:{green}) @price:[0 100]"}),
            (std::vector<std::string>{"2", "item:1", "item:2"}));
  EXPECT_EQ(keys({"items", "@price:[$lo $hi]", "PARAMS", "4", "lo", "20", "hi", "30", "DIALECT", "2"}),
            (std::vector<std::string>{"1", "item:2"}));
  EXPECT_EQ(keys({"items", "*", "FILTER", "price", "30", "50"}), (std::vector<std::string>{"1", "item:3"}));
  EXPECT_TRUE(run({"FT.SEARCH", "items", "hello"}).error);
  EXPECT_TRUE(run({"FT.SEARCH", "nope", "*"}).error);

  // Sorting and paging; the count is of every match.
  EXPECT_EQ(keys({"items", "*", "SORTBY", "price", "DESC", "LIMIT", "0", "2"}),
            (std::vector<std::string>{"4", "item:3", "item:2"}));
  EXPECT_EQ(keys({"items", "*", "SORTBY", "price", "LIMIT", "2", "5"}),
            (std::vector<std::string>{"4", "item:3", "item:4"}));  // no price: last
  EXPECT_EQ(keys({"items", "@color:{red}", "LIMIT", "0", "0"}), (std::vector<std::string>{"2"}));

  // Content: the whole hash, or RETURN (with AS).
  Value r = run({"FT.SEARCH", "items", "@price:[40 40]"});
  ASSERT_EQ(r.items.size(), 3u);
  EXPECT_EQ(r.items[0].text, "1");
  EXPECT_EQ(r.items[1].text, "item:3");
  EXPECT_EQ(r.items[2].items.size(), 4u);
  r = run({"FT.SEARCH", "items", "@sz:{m}", "RETURN", "3", "price", "AS", "p", "WITHSCORES"});
  ASSERT_EQ(r.items.size(), 4u);
  EXPECT_EQ(r.items[2].text, "1");
  ASSERT_EQ(r.items[3].items.size(), 2u);
  EXPECT_EQ(r.items[3].items[0].text, "p");
  EXPECT_EQ(r.items[3].items[1].text, "25.5");

  // Writes keep it current.
  text({"HSET", "item:2", "color", "red"});
  text({"HDEL", "item:1", "color"});
  text({"DEL", "item:3"});
  EXPECT_EQ(keys({"items", "@color:{red}"}), (std::vector<std::string>{"1", "item:2"}));
  EXPECT_EQ(keys({"items", "@color:{green}"}), (std::vector<std::string>{"0"}));
  EXPECT_EQ(keys({"items", "*"}).front(), "3");
  text({"PEXPIRE", "item:2", "1"});
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_EQ(keys({"items", "@color:{red}"}), (std::vector<std::string>{"0"}));

  // Introspection.
  auto info = items({"FT.INFO", "items"});
  auto at = std::find(info.begin(), info.end(), "num_docs");
  ASSERT_NE(at, info.end());
  EXPECT_EQ(*(at + 1), "2");
  EXPECT_EQ(items({"FT._LIST"}), std::vector<std::string>{"items"});
  text({"HSET", "item:5", "color", "Yellow,red"});
  EXPECT_EQ(items({"FT.TAGVALS", "items", "color"}), (std::vector<std::string>{"red", "yellow"}));
  EXPECT_TRUE(run({"FT.TAGVALS", "items", "price"}).error);
  // An FT index is not for IDX.QUERY.
  EXPECT_TRUE(run({"IDX.QUERY", "items", "color", "=", "red"}).error);

  EXPECT_EQ(text({"FT.DROPINDEX", "items", "DD"}), "OK");
  EXPECT_EQ(text({"EXISTS", "item:1", "item:4", "item:5", "other:1"}), "1");
  EXPECT_TRUE(items({"FT._LIST"}).empty());
}

TEST_F(Search, JsonDocumentsAndBackfill) {
  text({"JSON.SET", "doc:1", "$", R"({"name":"Ana","tags":["a","B"],"scores":[1,5],"address":{"city":"SP"}})"});
  text({"JSON.SET", "doc:2", "$", R"({"name":"Bia","tags":["b"],"scores":[7],"address":{"city":"RJ"}})"});
  text({"HSET", "doc:3", "tags", "a"});  // a hash: not in a JSON index
  ASSERT_EQ(text({"FT.CREATE", "docs", "ON", "JSON", "PREFIX", "1", "doc:", "SCHEMA", "$.tags[*]", "AS", "tag", "TAG",
                  "$.scores", "AS", "score", "NUMERIC", "$.address.city", "AS", "city", "TAG", "$.name", "AS", "name",
                  "TAG"}),
            "OK");
  ready("docs");
  EXPECT_EQ(keys({"docs", "@tag:{b}"}), (std::vector<std::string>{"2", "doc:1", "doc:2"}));
  EXPECT_EQ(keys({"docs", "@score:[5 6]"}), (std::vector<std::string>{"1", "doc:1"}));
  EXPECT_EQ(keys({"docs", "@tag:{a} @score:[6 +inf]"}), (std::vector<std::string>{"0"}));
  EXPECT_EQ(keys({"docs", "*"}), (std::vector<std::string>{"2", "doc:1", "doc:2"}));
  // Sorted by a multi-valued NUMERIC: by its smallest value going up.
  EXPECT_EQ(keys({"docs", "*", "SORTBY", "score"}), (std::vector<std::string>{"2", "doc:1", "doc:2"}));

  Value r = run({"FT.SEARCH", "docs", "@city:{rj}"});
  ASSERT_EQ(r.items.size(), 3u);
  ASSERT_EQ(r.items[2].items.size(), 2u);
  EXPECT_EQ(r.items[2].items[0].text, "$");
  r = run({"FT.SEARCH", "docs", "@city:{sp}", "RETURN", "2", "name", "$.address"});
  ASSERT_EQ(r.items.size(), 3u);
  ASSERT_EQ(r.items[2].items.size(), 4u);
  EXPECT_EQ(r.items[2].items[1].text, "Ana");  // as stored, not case folded
  EXPECT_EQ(r.items[2].items[3].text, R"({"city":"SP"})");

  // Array changes reach the index.
  text({"JSON.ARRAPPEND", "doc:2", "$.tags", R"("c")"});
  text({"JSON.ARRPOP", "doc:1", "$.tags"});
  EXPECT_EQ(keys({"docs", "@tag:{c}"}), (std::vector<std::string>{"1", "doc:2"}));
  EXPECT_EQ(keys({"docs", "@tag:{b}"}), (std::vector<std::string>{"1", "doc:2"}));
  text({"JSON.DEL", "doc:2", "$.scores"});
  EXPECT_EQ(keys({"docs", "@score:[-inf +inf]"}), (std::vector<std::string>{"1", "doc:1"}));
  EXPECT_EQ(keys({"docs", "*"}).front(), "2");  // still in the index without the field
  text({"SET", "doc:1", "plain string"});
  EXPECT_EQ(keys({"docs", "*"}), (std::vector<std::string>{"1", "doc:2"}));

  // SKIPINITIALSCAN: only later writes.
  ASSERT_EQ(text({"FT.CREATE", "later", "ON", "JSON", "SKIPINITIALSCAN", "SCHEMA", "$.name", "AS", "name", "TAG"}),
            "OK");
  EXPECT_EQ(keys({"later", "*"}), (std::vector<std::string>{"0"}));
  text({"JSON.SET", "doc:9", "$", R"({"name":"Zé"})"});
  EXPECT_EQ(keys({"later", "@name:{zé}"}), (std::vector<std::string>{"1", "doc:9"}));
}

TEST_F(Search, CompoundIndexesWithSeveralValues) {
  ASSERT_EQ(text({"IDX.CREATE", "byskill", "PREFIX", "1", "dev:", "SCHEMA", "skills", "TAG", "SEPARATOR", ",",
                  "level", "NUMERIC"}),
            "OK");
  // Field names of the same length are not mistaken for duplicates.
  EXPECT_EQ(text({"IDX.CREATE", "same", "SCHEMA", "color", "TAG", "price", "NUMERIC"}), "OK");
  EXPECT_EQ(text({"IDX.CREATE", "dup", "SCHEMA", "color", "TAG", "color", "NUMERIC"}),
            "ERR duplicate field 'color'");
  text({"HSET", "dev:1", "skills", "c++,go", "level", "3"});
  text({"HSET", "dev:2", "skills", "go", "level", "5"});
  ready("byskill", false);
  EXPECT_EQ(items({"IDX.QUERY", "byskill", "skills", "=", "go", "NOCONTENT"}),
            (std::vector<std::string>{"dev:1", "dev:2"}));
  EXPECT_EQ(items({"IDX.QUERY", "byskill", "skills", "=", "c++", "level", ">", "2", "NOCONTENT"}),
            std::vector<std::string>{"dev:1"});
  // LIMIT offset 0 is an empty page, with or without content.
  EXPECT_TRUE(items({"IDX.QUERY", "byskill", "skills", "=", "go", "LIMIT", "0", "0", "NOCONTENT"}).empty());
  EXPECT_TRUE(items({"IDX.QUERY", "byskill", "skills", "=", "go", "LIMIT", "1", "0"}).empty());
  // A scan that meets a key once per value reports it once.
  EXPECT_EQ(text({"IDX.COUNT", "byskill"}), "2");
  EXPECT_EQ(items({"IDX.QUERY", "byskill", "level", ">=", "0", "NOCONTENT"}).size(), 2u);
  EXPECT_EQ(items({"IDX.QUERY", "byskill", "skills", "=", "go", "skills", "=", "c++", "NOCONTENT"}),
            std::vector<std::string>{"dev:1"});
  text({"HSET", "dev:1", "skills", "rust"});
  EXPECT_EQ(items({"IDX.QUERY", "byskill", "skills", "=", "go", "NOCONTENT"}), std::vector<std::string>{"dev:2"});
  auto info = items({"IDX.INFO", "byskill"});
  EXPECT_NE(std::find(info.begin(), info.end(), "compound"), info.end());
  // An IDX index is not for FT.SEARCH.
  EXPECT_TRUE(run({"FT.SEARCH", "byskill", "*"}).error);

  // JSON arrays in a compound index.
  ASSERT_EQ(text({"IDX.CREATE", "posts", "ON", "JSON", "PREFIX", "1", "post:", "SCHEMA", "$.tags", "AS", "tag", "TAG",
                  "$.likes", "AS", "likes", "NUMERIC"}),
            "OK");
  ready("posts", false);
  text({"JSON.SET", "post:1", "$", R"({"tags":["db","cpp"],"likes":10})"});
  text({"JSON.SET", "post:2", "$", R"({"tags":["db"],"likes":3})"});
  EXPECT_EQ(items({"IDX.QUERY", "posts", "tag", "=", "db", "NOCONTENT"}),
            (std::vector<std::string>{"post:2", "post:1"}));  // index order: by likes
  EXPECT_EQ(items({"IDX.QUERY", "posts", "tag", "=", "cpp", "NOCONTENT"}), std::vector<std::string>{"post:1"});
  EXPECT_EQ(text({"IDX.COUNT", "posts"}), "2");
  text({"JSON.ARRAPPEND", "post:2", "$.tags", R"("cpp")"});
  EXPECT_EQ(text({"IDX.COUNT", "posts", "tag", "=", "cpp"}), "2");
}
