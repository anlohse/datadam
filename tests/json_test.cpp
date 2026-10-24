// The JSON type: paths, and the JSON.* commands.
#include <gtest/gtest.h>

#include <filesystem>
#include <algorithm>
#include <random>
#include <thread>

#include "cache/object_cache.h"
#include "db/database.h"
#include "json/json.h"
#include "json/json_path.h"
#include "index/index_def.h"
#include "server/redis_commands.h"
#include "storage/rocks_storage.h"

using namespace datadam;
namespace fs = std::filesystem;

namespace {

Json doc_of(std::string_view text) {
  std::string error;
  auto v = parse_json(text, &error);
  EXPECT_TRUE(v) << error;
  return v ? *v : Json();
}

// The values a path selects, as compact JSON.
std::vector<std::string> sel(std::string_view text, std::string_view path) {
  Json doc = doc_of(text);
  std::string error;
  auto p = JsonPath::parse(path, &error);
  EXPECT_TRUE(p) << error;
  std::vector<std::string> out;
  if (!p) return out;
  for (const Json* v : p->select(std::as_const(doc))) out.push_back(dump_json(*v));
  return out;
}

using V = std::vector<std::string>;

const char* kStore = R"({"store":{"book":[
  {"category":"reference","author":"Rees","title":"Sayings","price":8.95},
  {"category":"fiction","author":"Waugh","title":"Sword","price":12.99},
  {"category":"fiction","author":"Melville","title":"Moby Dick","isbn":"0-553","price":8.99}],
  "bicycle":{"color":"red","price":19.95}}})";

}  // namespace

TEST(JsonPath, Selectors) {
  EXPECT_EQ(sel(R"({"a":{"b":1}})", "$.a.b"), V{"1"});
  EXPECT_EQ(sel(R"({"a":{"b":1}})", "$['a']['b']"), V{"1"});
  EXPECT_EQ(sel(R"({"a":[1,2,3]})", "$.a[0]"), V{"1"});
  EXPECT_EQ(sel(R"({"a":[1,2,3]})", "$.a[-1]"), V{"3"});
  EXPECT_EQ(sel(R"({"a":[1,2,3]})", "$.a[*]"), (V{"1", "2", "3"}));
  EXPECT_EQ(sel(R"({"a":[1,2,3,4]})", "$.a[1:3]"), (V{"2", "3"}));
  EXPECT_EQ(sel(R"({"a":[1,2,3,4]})", "$.a[::-1]"), (V{"4", "3", "2", "1"}));
  EXPECT_EQ(sel(R"({"a":[1,2,3,4]})", "$.a[0,2]"), (V{"1", "3"}));
  EXPECT_EQ(sel(R"({"a":1,"b":2})", "$.*"), (V{"1", "2"}));
  EXPECT_EQ(sel(R"({"a":1})", "$"), V{R"({"a":1})"});
  EXPECT_TRUE(sel(R"({"a":1})", "$.b").empty());
  EXPECT_EQ(sel(kStore, "$..author"), (V{"\"Rees\"", "\"Waugh\"", "\"Melville\""}));
  EXPECT_EQ(sel(kStore, "$.store..price"), (V{"8.95", "12.99", "8.99", "19.95"}));
  EXPECT_EQ(sel(kStore, "$..book[2].title"), V{"\"Moby Dick\""});
}

TEST(JsonPath, Filters) {
  EXPECT_EQ(sel(kStore, "$..book[?(@.price < 10)].title"), (V{"\"Sayings\"", "\"Moby Dick\""}));
  EXPECT_EQ(sel(kStore, "$..book[?(@.isbn)].author"), V{"\"Melville\""});
  EXPECT_EQ(sel(kStore, "$..book[?(@.category == 'fiction' && @.price > 10)].author"), V{"\"Waugh\""});
  EXPECT_EQ(sel(kStore, "$..book[?(@.author == \"Rees\" || @.author == \"Waugh\")].price"), (V{"8.95", "12.99"}));
  EXPECT_EQ(sel(kStore, "$..book[?(!(@.price < 10))].author"), V{"\"Waugh\""});
  EXPECT_EQ(sel(kStore, "$..book[?(@.price > $.store.bicycle.price)]").size(), 0u);
  EXPECT_EQ(sel(R"([1,5,3])", "$[?(@ > 2)]"), (V{"5", "3"}));
}

TEST(JsonPath, LegacyAndErrors) {
  EXPECT_EQ(sel(R"({"a":{"b":[1,2]}})", ".a.b[1]"), V{"2"});
  EXPECT_EQ(sel(R"({"a":{"b":[1,2]}})", "a.b"), V{"[1,2]"});
  EXPECT_EQ(sel(R"({"a":1})", "."), V{R"({"a":1})"});
  EXPECT_EQ(sel(R"({"a":[1,2]})", ".a[*]"), V{"1"});  // legacy: the first match
  std::string error;
  for (std::string_view bad : {"$.a[", "$.a[x]", "$[?(@.a ==)]", "$..", "$.a]", ".a..b"}) {
    error.clear();
    EXPECT_FALSE(JsonPath::parse(bad, &error)) << bad;
    EXPECT_FALSE(error.empty()) << bad;
  }
}

TEST(JsonPath, CreateAndErase) {
  Json doc = doc_of(R"({"a":{"x":1},"b":{"x":2},"c":3})");
  std::string error;
  size_t created = 0;
  auto p = JsonPath::parse("$.*.y", &error);
  auto made = p->select_or_create(doc, &created);
  EXPECT_EQ(created, 2u);  // in a and b; c is not an object
  EXPECT_EQ(made.size(), 2u);
  auto x = JsonPath::parse("$..x", &error);
  EXPECT_EQ(x->erase(doc), 2u);
  EXPECT_EQ(dump_json(doc), R"({"a":{"y":null},"b":{"y":null},"c":3})");
  Json nested = doc_of(R"({"a":{"a":{"a":1}}})");
  EXPECT_EQ(JsonPath::parse("$..a", &error)->erase(nested), 1u);  // inner ones go with the outer
  EXPECT_EQ(dump_json(nested), "{}");
}

TEST(Json, DepthLimitAndFormat) {
  std::string error;
  std::string deep(200, '[');
  deep += std::string(200, ']');
  EXPECT_FALSE(parse_json(deep, &error));
  EXPECT_NE(error.find("128"), std::string::npos);
  Json d = doc_of(R"({"a":[1,{"b":true}],"e":{}})");
  EXPECT_EQ(dump_json(d), R"({"a":[1,{"b":true}],"e":{}})");
  JsonFormat f{"  ", "\n", " "};
  EXPECT_EQ(dump_json(d, f), "{\n  \"a\": [\n    1,\n    {\n      \"b\": true\n    }\n  ],\n  \"e\": {}\n}");
  EXPECT_EQ(json_type_name(doc_of("1")), "integer");
  EXPECT_EQ(json_type_name(doc_of("1.5")), "number");
}

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

class JsonCommands : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("datadam_json_" + std::to_string(std::random_device{}()));
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

  fs::path dir_;
  Config config_;
  ServerStats stats_;
  ClientState client_;
  std::unique_ptr<RocksStorage> storage_;
  std::unique_ptr<ObjectCache> cache_;
  std::unique_ptr<Database> db_;
  std::unique_ptr<ServerContext> ctx_;
};

}  // namespace

TEST_F(JsonCommands, SetGetDelType) {
  EXPECT_EQ(text({"JSON.SET", "doc", "$", R"({"a":2,"nested":{"a":4,"b":null}})"}), "OK");
  EXPECT_EQ(text({"JSON.GET", "doc"}), R"({"a":2,"nested":{"a":4,"b":null}})");
  text({"SET", "plain", "x"});
  for (const char* type : {"ReJSON-RL", "rejson-rl"}) {  // SCAN's TYPE, as TYPE names it, in any case
    Value scan = run({"SCAN", "0", "TYPE", type});
    ASSERT_EQ(scan.items.size(), 2u);
    ASSERT_EQ(scan.items[1].items.size(), 1u) << type;
    EXPECT_EQ(scan.items[1].items[0].text, "doc");
  }
  EXPECT_EQ(text({"JSON.GET", "doc", "$..a"}), "[2,4]");
  EXPECT_EQ(text({"JSON.GET", "doc", ".nested.a"}), "4");
  EXPECT_EQ(text({"JSON.GET", "doc", "$.a", "$..b"}), R"({"$.a":[2],"$..b":[null]})");
  EXPECT_TRUE(run({"JSON.GET", "doc", ".missing"}).error);
  EXPECT_EQ(text({"JSON.GET", "doc", "$.missing"}), "[]");
  EXPECT_TRUE(run({"JSON.GET", "nokey"}).null);
  EXPECT_EQ(text({"JSON.GET", "doc", "INDENT", "\t", "NEWLINE", "\n", "SPACE", " ", "$.nested"}),
            "[\n\t{\n\t\t\"a\": 4,\n\t\t\"b\": null\n\t}\n]");
  // Setting: replace, add a member, NX / XX, and only at the root for a new key.
  EXPECT_EQ(text({"JSON.SET", "doc", "$..a", "3"}), "OK");
  EXPECT_EQ(text({"JSON.GET", "doc", "$..a"}), "[3,3]");
  EXPECT_EQ(text({"JSON.SET", "doc", "$.new", R"("x")"}), "OK");
  EXPECT_TRUE(run({"JSON.SET", "doc", "$.new", "1", "NX"}).null);
  EXPECT_TRUE(run({"JSON.SET", "doc", "$.other", "1", "XX"}).null);
  EXPECT_TRUE(run({"JSON.SET", "doc", "$.no.such", "1"}).null);  // its parent is missing
  EXPECT_TRUE(run({"JSON.SET", "fresh", "$.a", "1"}).error);
  EXPECT_TRUE(run({"JSON.SET", "doc", "$", "{bad json"}).error);
  EXPECT_EQ(items({"JSON.TYPE", "doc", "$..a"}), (V{"integer", "integer"}));
  EXPECT_EQ(text({"JSON.TYPE", "doc"}), "object");
  EXPECT_EQ(text({"TYPE", "doc"}), "ReJSON-RL");
  // A JSON key is not a string, and other types are not JSON.
  EXPECT_TRUE(run({"GET", "doc"}).error);
  EXPECT_EQ(text({"SET", "str", "v"}), "OK");
  EXPECT_TRUE(run({"JSON.GET", "str"}).error);
  // Deleting paths, then the key.
  EXPECT_EQ(text({"JSON.DEL", "doc", "$..a"}), "2");
  EXPECT_EQ(text({"JSON.GET", "doc"}), R"({"nested":{"b":null},"new":"x"})");
  EXPECT_EQ(text({"JSON.FORGET", "doc"}), "1");
  EXPECT_EQ(text({"EXISTS", "doc"}), "0");
}

TEST_F(JsonCommands, NumbersStringsAndBooleans) {
  EXPECT_EQ(text({"JSON.SET", "d", "$", R"({"a":"b","b":[{"a":2},{"a":5},{"a":"c"}],"n":1,"f":true})"}), "OK");
  EXPECT_EQ(text({"JSON.NUMINCRBY", "d", "$..a", "2"}), "[null,4,7,null]");
  EXPECT_EQ(text({"JSON.NUMINCRBY", "d", ".n", "1.5"}), "2.5");
  EXPECT_EQ(text({"JSON.NUMMULTBY", "d", "$.b[0].a", "3"}), "[12]");
  EXPECT_TRUE(run({"JSON.NUMINCRBY", "d", ".a", "1"}).error);  // a string
  EXPECT_TRUE(run({"JSON.NUMINCRBY", "nokey", ".a", "1"}).error);
  EXPECT_EQ(items({"JSON.STRAPPEND", "d", "$..a", R"("z")"}), (V{"2", "nil", "nil", "2"}));
  EXPECT_EQ(text({"JSON.STRLEN", "d", ".a"}), "2");
  EXPECT_EQ(items({"JSON.TOGGLE", "d", "$.f"}), V{"0"});
  EXPECT_EQ(text({"JSON.TOGGLE", "d", ".f"}), "true");
}

TEST_F(JsonCommands, ArraysAndObjects) {
  EXPECT_EQ(text({"JSON.SET", "d", "$", R"({"arr":[1,2,3],"obj":{"x":1,"y":2},"s":"t"})"}), "OK");
  EXPECT_EQ(items({"JSON.ARRAPPEND", "d", "$.arr", "4", R"("five")"}), V{"5"});
  EXPECT_EQ(text({"JSON.ARRINSERT", "d", ".arr", "0", "0"}), "6");
  EXPECT_EQ(text({"JSON.ARRINSERT", "d", ".arr", "-1", "4.5"}), "7");
  EXPECT_EQ(text({"JSON.GET", "d", ".arr"}), R"([0,1,2,3,4,4.5,"five"])");
  EXPECT_TRUE(run({"JSON.ARRINSERT", "d", ".arr", "99", "1"}).error);
  EXPECT_EQ(text({"JSON.ARRLEN", "d", ".arr"}), "7");
  EXPECT_EQ(items({"JSON.ARRLEN", "d", "$.*"}), (V{"7", "nil", "nil"}));
  EXPECT_EQ(text({"JSON.ARRPOP", "d", ".arr"}), R"("five")");
  EXPECT_EQ(text({"JSON.ARRPOP", "d", ".arr", "0"}), "0");
  EXPECT_EQ(text({"JSON.ARRINDEX", "d", ".arr", "3"}), "2");
  EXPECT_EQ(text({"JSON.ARRINDEX", "d", ".arr", "9"}), "-1");
  EXPECT_EQ(text({"JSON.ARRTRIM", "d", ".arr", "1", "-2"}), "3");
  EXPECT_EQ(text({"JSON.GET", "d", ".arr"}), "[2,3,4]");
  EXPECT_EQ(items({"JSON.OBJKEYS", "d", ".obj"}), (V{"x", "y"}));
  EXPECT_EQ(text({"JSON.OBJLEN", "d", ".obj"}), "2");
  EXPECT_EQ(text({"JSON.CLEAR", "d", "$.*"}), "2");  // the array and the object; the string stays
  EXPECT_EQ(text({"JSON.GET", "d"}), R"({"arr":[],"obj":{},"s":"t"})");
  EXPECT_TRUE(run({"JSON.ARRPOP", "d", ".arr"}).null);
}

TEST_F(JsonCommands, MergeMultiKeyAndTtl) {
  EXPECT_EQ(text({"JSON.SET", "d", "$", R"({"a":{"b":1,"c":2}})"}), "OK");
  EXPECT_EQ(text({"JSON.MERGE", "d", "$", R"({"a":{"b":null,"d":3}})"}), "OK");
  EXPECT_EQ(text({"JSON.GET", "d"}), R"({"a":{"c":2,"d":3}})");
  EXPECT_EQ(text({"JSON.MSET", "d", "$.a.c", "9", "e", "$", R"({"x":1})"}), "OK");
  EXPECT_EQ(items({"JSON.MGET", "d", "e", "nokey", "$..c"}), (V{"[9]", "[]", "nil"}));
  EXPECT_EQ(items({"JSON.MGET", "d", "e", ".x"}), (V{"nil", "1"}));
  // A document keeps its TTL across JSON writes, like any record.
  EXPECT_EQ(text({"EXPIRE", "d", "100"}), "1");
  EXPECT_EQ(text({"JSON.SET", "d", "$.a.c", "10"}), "OK");
  EXPECT_NE(text({"TTL", "d"}), "-1");
  EXPECT_EQ(text({"RENAME", "d", "d2"}), "OK");
  EXPECT_EQ(text({"JSON.GET", "d2", ".a.c"}), "10");
}

TEST(JsonIndexDef, EncodesSourceAndPaths) {
  IndexDef def;
  def.id = 7;
  def.name = "users";
  def.source = IndexSource::Json;
  def.prefixes = {"user:"};
  def.fields = {{"city", IndexFieldType::Tag, "$.address.city"}, {"age", IndexFieldType::Numeric, ".age"}};
  ASSERT_EQ(def.compile(), "");
  IndexDef back;
  ASSERT_TRUE(IndexDef::decode(def.encode(), &back));
  EXPECT_TRUE(back.json());
  EXPECT_EQ(back.fields[0].path, "$.address.city");
  EXPECT_EQ(back.fields[1].name, "age");
  Json doc = doc_of(R"({"address":{"city":"SP"},"age":30,"ok":true})");
  EXPECT_EQ(back.json_values(0, doc), std::vector<std::string>{"SP"});
  EXPECT_EQ(back.json_values(1, doc), std::vector<std::string>{"30"});
  EXPECT_EQ(back.json_tuples(doc).size(), 1u);
  // An array gives one value per element, so one tuple per combination.
  EXPECT_EQ(back.json_values(0, doc_of(R"({"address":{"city":["SP","RJ",{"x":1},"SP"]}})")),
            (std::vector<std::string>{"SP", "RJ"}));
  EXPECT_EQ(back.json_tuples(doc_of(R"({"address":{"city":["SP","RJ"]},"age":[30,31]})")).size(), 4u);
  EXPECT_TRUE(back.json_tuples(doc_of(R"({"address":{"city":[]},"age":30})")).empty());
  def.fields[0].path = "$.a[";
  EXPECT_NE(def.compile(), "");
}

class JsonIndexes : public JsonCommands {
 protected:
  void ready(const std::string& name) {
    for (int i = 0; i < 500; ++i) {
      auto info = items({"IDX.INFO", name});
      for (size_t k = 0; k + 1 < info.size(); ++k) {
        if (info[k] == "state" && info[k + 1] == "ready") return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    FAIL() << "index " << name << " not ready";
  }
};

TEST_F(JsonIndexes, QueryAndMaintenance) {
  EXPECT_EQ(text({"JSON.SET", "user:1", "$", R"({"name":"ana","address":{"city":"SP"},"age":30})"}), "OK");
  EXPECT_EQ(text({"JSON.SET", "user:2", "$", R"({"name":"bia","address":{"city":"RJ"},"age":25})"}), "OK");
  EXPECT_EQ(text({"JSON.SET", "user:3", "$", R"({"name":"cid","address":{"city":"SP"},"age":41})"}), "OK");
  EXPECT_EQ(text({"JSON.SET", "user:4", "$", R"({"name":"dan"})"}), "OK");  // no city: not indexed
  EXPECT_EQ(text({"HSET", "user:5", "city", "SP", "age", "1"}), "2");    // a hash: not for a JSON index
  EXPECT_EQ(text({"IDX.CREATE", "users", "ON", "JSON", "PREFIX", "1", "user:", "SCHEMA", "$.address.city", "AS", "city",
                  "TAG", "$.age", "AS", "age", "NUMERIC"}),
            "OK");
  ready("users");
  EXPECT_EQ(items({"IDX.QUERY", "users", "city", "=", "SP", "NOCONTENT"}), (V{"user:1", "user:3"}));
  EXPECT_EQ(text({"IDX.COUNT", "users", "city", "=", "SP"}), "2");
  Value hit = run({"IDX.QUERY", "users", "city", "=", "SP", "age", ">", "35"});
  ASSERT_EQ(hit.items.size(), 2u);
  EXPECT_EQ(hit.items[0].text, "user:3");
  ASSERT_EQ(hit.items[1].items.size(), 2u);
  EXPECT_EQ(hit.items[1].items[0].text, "$");
  EXPECT_EQ(hit.items[1].items[1].text, R"({"name":"cid","address":{"city":"SP"},"age":41})");
  Value ret = run({"IDX.QUERY", "users", "city", "=", "SP", "DESC", "RETURN", "2", "age", "$.name"});
  ASSERT_EQ(ret.items.size(), 4u);
  EXPECT_EQ(ret.items[0].text, "user:3");
  EXPECT_EQ(ret.items[1].items[1].text, "41");
  EXPECT_EQ(ret.items[1].items[3].text, "cid");
  // Conditions on other paths filter what the index found.
  EXPECT_EQ(items({"IDX.QUERY", "users", "city", "=", "SP", "$.name", "=", "ana", "NOCONTENT"}), V{"user:1"});
  EXPECT_EQ(items({"IDX.QUERY", "users", "city", "=", "SP", "name", "=", "cid", "NOCONTENT"}), V{"user:3"});

  // Every write of a document keeps its entries right.
  EXPECT_EQ(text({"JSON.SET", "user:2", "$.address.city", R"("SP")"}), "OK");
  EXPECT_EQ(text({"IDX.COUNT", "users", "city", "=", "SP"}), "3");
  EXPECT_EQ(text({"JSON.NUMINCRBY", "user:2", "$.age", "20"}), "[45]");
  EXPECT_EQ(items({"IDX.QUERY", "users", "city", "=", "SP", "age", ">", "42", "NOCONTENT"}), V{"user:2"});
  EXPECT_EQ(text({"JSON.DEL", "user:1", "$.address"}), "1");
  EXPECT_EQ(items({"IDX.QUERY", "users", "city", "=", "SP", "NOCONTENT"}), (V{"user:3", "user:2"}));  // by age
  EXPECT_EQ(text({"DEL", "user:3"}), "1");
  EXPECT_EQ(text({"SET", "user:2", "now a string"}), "OK");
  EXPECT_EQ(text({"IDX.COUNT", "users", "city", "=", "SP"}), "0");
  EXPECT_EQ(text({"JSON.SET", "user:7", "$", R"({"address":{"city":"BH"},"age":3})"}), "OK");
  EXPECT_EQ(text({"RENAME", "user:7", "user:8"}), "OK");
  EXPECT_EQ(items({"IDX.QUERY", "users", "city", "=", "BH", "NOCONTENT"}), V{"user:8"});
  EXPECT_EQ(text({"EXPIRE", "user:8", "100"}), "1");
  EXPECT_EQ(items({"IDX.QUERY", "users", "city", "=", "BH", "RETURN", "1", "age"})[0], "user:8");
  EXPECT_EQ(text({"JSON.SET", "user:8", "$", R"({"age":3})"}), "OK");  // the city is gone
  EXPECT_EQ(text({"IDX.COUNT", "users", "city", "=", "BH"}), "0");
  // A collection command that replaces a document takes its entries along.
  EXPECT_EQ(text({"JSON.SET", "user:9", "$", R"({"address":{"city":"CX"},"age":9})"}), "OK");
  EXPECT_EQ(text({"SADD", "src", "a"}), "1");
  EXPECT_EQ(text({"SUNIONSTORE", "user:9", "src"}), "1");
  EXPECT_EQ(text({"IDX.COUNT", "users", "city", "=", "CX"}), "0");
  // IDX.INFO tells it is over JSON.
  auto info = items({"IDX.INFO", "users"});
  EXPECT_NE(std::find(info.begin(), info.end(), "JSON"), info.end());
}

TEST_F(JsonIndexes, Unique) {
  EXPECT_EQ(text({"IDX.CREATE", "emails", "ON", "JSON", "PREFIX", "1", "acct:", "SCHEMA", "$.email", "TAG", "UNIQUE"}),
            "OK");
  ready("emails");
  EXPECT_EQ(text({"JSON.SET", "acct:1", "$", R"({"email":"a@x"})"}), "OK");
  Value clash = run({"JSON.SET", "acct:2", "$", R"({"email":"a@x"})"});
  EXPECT_TRUE(clash.error);
  EXPECT_NE(clash.text.find("UNIQUE"), std::string::npos) << clash.text;
  EXPECT_EQ(text({"EXISTS", "acct:2"}), "0");  // nothing was written
  EXPECT_EQ(run({"IDX.GET", "emails", "a@x"}).items[0].text, "acct:1");
  EXPECT_EQ(text({"JSON.SET", "acct:1", "$.email", R"("b@x")"}), "OK");
  EXPECT_EQ(text({"JSON.SET", "acct:2", "$", R"({"email":"a@x"})"}), "OK");  // free again
  EXPECT_EQ(run({"IDX.GET", "emails", "a@x"}).items[0].text, "acct:2");
  EXPECT_EQ(text({"RENAME", "acct:2", "acct:3"}), "OK");  // its own value moves with it
  EXPECT_EQ(run({"IDX.GET", "emails", "a@x"}).items[0].text, "acct:3");
}
