// Config store unit test: host:port parse/format (IPv4/IPv6/bare) and the
// add -> edit -> delete -> save JSON -> reload round-trip.

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>

#include "config/DeviceConfigStore.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

static void testParseFormat()
{
    // IPv4
    HostPort a = parseHostPort(QStringLiteral("192.168.1.10:2222"));
    CHECK(a.host == QStringLiteral("192.168.1.10") && a.port == 2222);
    // bare host -> default
    HostPort b = parseHostPort(QStringLiteral("example.com"));
    CHECK(b.host == QStringLiteral("example.com") && b.port == 22);
    // [IPv6]:port
    HostPort c = parseHostPort(QStringLiteral("[fd00::1]:2200"));
    CHECK(c.host == QStringLiteral("fd00::1") && c.port == 2200);
    // bare IPv6 -> default port, no split
    HostPort d = parseHostPort(QStringLiteral("fd00::1"));
    CHECK(d.host == QStringLiteral("fd00::1") && d.port == 22);

    // format round-trips
    CHECK(formatHostPort(QStringLiteral("192.168.1.10"), 22) == QStringLiteral("192.168.1.10:22"));
    CHECK(formatHostPort(QStringLiteral("fd00::1"), 3389) == QStringLiteral("[fd00::1]:3389"));
}

static void testCrudRoundTrip()
{
    // 夹具缺失时直接跳过而不是往下走：后面的 find() 会拿到 nullptr，
    // 对它解引用就是段错误（夹具只存在于开发机的 /tmp 下）。
    if (!QFile::exists(QStringLiteral("/tmp/cubeshell_test/config_p4.dat"))) {
        qWarning() << "SKIP: missing fixture /tmp/cubeshell_test/config_p4.dat";
        return;
    }
    DeviceConfigStore store;
    CHECK(store.load(QStringLiteral("/tmp/cubeshell_test/config_p4.dat")));
    const int base = store.count();
    CHECK(base == 3);

    // The pickle stores host as "host:port".
    const DeviceEntry *web = store.find(QStringLiteral("web服务器"));
    CHECK(web != nullptr);
    CHECK(web->hostPort().host == QStringLiteral("192.168.1.10"));

    // ADD
    DeviceEntry added;
    added.name = QStringLiteral("new-box");
    added.username = QStringLiteral("deploy");
    added.password = QStringLiteral("pw");
    added.host = formatHostPort(QStringLiteral("10.0.0.5"), 2222);
    added.port = 2222;
    store.addDevice(added);
    CHECK(store.count() == base + 1);
    CHECK(store.find(QStringLiteral("new-box"))->hostPort().port == 2222);

    // EDIT (rename + change key auth)
    DeviceEntry edited = *store.find(QStringLiteral("new-box"));
    store.removeDevice(QStringLiteral("new-box"));
    edited.name = QStringLiteral("renamed-box");
    edited.password.clear();
    edited.keyType = QStringLiteral("Ed25519Key");
    edited.keyFile = QStringLiteral("/home/deploy/.ssh/id_ed25519");
    store.addDevice(edited);
    CHECK(store.find(QStringLiteral("new-box")) == nullptr);
    const DeviceEntry *rb = store.find(QStringLiteral("renamed-box"));
    CHECK(rb && rb->usesKey());

    // DELETE
    CHECK(store.removeDevice(QStringLiteral("renamed-box")));
    CHECK(store.count() == base);

    // SAVE + RELOAD JSON
    const QString jsonPath = QDir::temp().filePath(QStringLiteral("cubeshell_crud.json"));
    CHECK(store.saveJson(jsonPath));
    DeviceConfigStore reloaded;
    CHECK(reloaded.loadJson(jsonPath));
    CHECK(reloaded.count() == store.count());
    const DeviceEntry *rw = reloaded.find(QStringLiteral("web服务器"));
    CHECK(rw && rw->username == QStringLiteral("root"));

    // 这份 store 来自 config.dat（pickle），inlinePasswords 为 true ——
    // 迁移窗口期内保存就必须继续写明文。翻闸门 + 再存一次，才是新格式。
    QFile f0(jsonPath);
    CHECK(f0.open(QIODevice::ReadOnly));
    CHECK(f0.readAll().contains("\"password\""));   // 窗口期：明文仍在
    f0.close();

    store.setInlinePasswords(false);                // 迁移验证通过后翻闸门
    CHECK(store.saveJson(jsonPath));

    // 密码绝不进 JSON：直接读文件字节，不能用 loadJson 间接验证
    //（loadJson 只认键名，手抖把 "password" 拼成 "passwrod" 它就看不出来了）。
    QFile f(jsonPath);
    CHECK(f.open(QIODevice::ReadOnly));
    const QByteArray raw = f.readAll();
    CHECK(!raw.contains("\"password\""));
    // id 必须在：它是钥匙串里密码的唯一索引。
    CHECK(raw.contains("\"id\""));
}

// 分组内展示顺序：按创建时间升序，后创建的排后面；旧版无时间戳（0）的
// 条目垫在最前面按名字排。落盘往返后顺序不变。
static void testCreationOrder()
{
    DeviceConfigStore store;
    const auto add = [&store](const QString &name, qint64 createdAt) {
        DeviceEntry e;
        e.name = name;
        e.createdAt = createdAt;
        e.host = formatHostPort(QStringLiteral("10.0.0.1"), 22);
        store.addDevice(e);
    };
    // 插入顺序与创建时间刻意错开：排序看的是 createdAt，不是 QHash 的插入序。
    add(QStringLiteral("zz"), 2000);
    add(QStringLiteral("aa"), 1000);
    add(QStringLiteral("mm"), 0);   // 旧版条目：无时间戳
    add(QStringLiteral("bb"), 0);

    const QList<DeviceEntry> devs = store.devices();
    CHECK(devs.size() == 4);
    // 旧版条目（0）垫前，同组之间按名字排；时间升序在后。
    CHECK(devs[0].name == QStringLiteral("bb"));
    CHECK(devs[1].name == QStringLiteral("mm"));
    CHECK(devs[2].name == QStringLiteral("aa"));
    CHECK(devs[3].name == QStringLiteral("zz"));

    // 保存 + 重载：createdAt 随 JSON 带走，顺序跨启动稳定。
    const QString jsonPath = QDir::temp().filePath(QStringLiteral("cubeshell_order.json"));
    CHECK(store.saveJson(jsonPath));
    DeviceConfigStore reloaded;
    CHECK(reloaded.loadJson(jsonPath));
    const QList<DeviceEntry> again = reloaded.devices();
    CHECK(again.size() == 4);
    for (int i = 0; i < again.size(); ++i)
        CHECK(again[i].name == devs[i].name);
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testParseFormat();
    testCrudRoundTrip();
    testCreationOrder();
    qInfo() << (failures == 0 ? "ALL PASS" : "FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
