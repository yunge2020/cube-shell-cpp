// Config module unit test: GroupManager CRUD + JSON round-trip (Python
// groups.json compatible), FrequentlyUsedCommands load/save/filter,
// ConfigUtil JSON/TOML loading against the real conf/ samples, and
// GlobalState path resolution. Secrets is exercised at compile/link level
// only (no real Keychain writes).

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "config/ConfigUtil.h"
#include "config/FrequentlyUsedCommands.h"
#include "config/GlobalState.h"
#include "config/GroupManager.h"
#include "config/Secrets.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// Real shared conf/ directory, resolved relative to this source file
// (tests -> ../conf), so the test works from any build dir.
static QString confDir()
{
    return QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
           + QStringLiteral("/../conf");
}

static void testGroupManager()
{
    const QString path = QDir::temp().filePath(QStringLiteral("cubeshell_groups_test.json"));
    QFile::remove(path);
    GroupManager gm(path);

    // Missing file -> empty structure, never an error.
    GroupData empty = gm.loadGroups();
    CHECK(empty.groups.isEmpty() && empty.deviceGroupMap.isEmpty());

    // CREATE (+ duplicate rejected)
    CHECK(gm.createGroup(QStringLiteral("华东地区")));
    CHECK(gm.createGroup(QStringLiteral("华南地区")));
    CHECK(!gm.createGroup(QStringLiteral("华东地区")));

    // Assign devices.
    gm.moveDeviceToGroup(QStringLiteral("设备A"), QStringLiteral("华东地区"));
    gm.moveDeviceToGroup(QStringLiteral("设备B"), QStringLiteral("华南地区"));
    gm.moveDeviceToGroup(QStringLiteral("设备C"), QStringLiteral("华东地区"));
    CHECK(gm.deviceGroup(QStringLiteral("设备A")) == QStringLiteral("华东地区"));
    CHECK(gm.deviceGroup(QStringLiteral("不存在")).isEmpty());

    // On-disk shape must match the Python format exactly.
    {
        QFile f(path);
        CHECK(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        CHECK(root.value(QStringLiteral("groups")).isArray());
        CHECK(root.value(QStringLiteral("device_group_map")).isObject());
        CHECK(root.value(QStringLiteral("groups")).toArray().size() == 2);
        CHECK(root.value(QStringLiteral("device_group_map")).toObject()
                  .value(QStringLiteral("设备A")).toString() == QStringLiteral("华东地区"));
    }

    // RENAME updates the map references too.
    CHECK(gm.renameGroup(QStringLiteral("华东地区"), QStringLiteral("华东")));
    CHECK(!gm.renameGroup(QStringLiteral("不存在"), QStringLiteral("x")));
    CHECK(!gm.renameGroup(QStringLiteral("华南地区"), QStringLiteral("华东"))); // 已存在
    CHECK(gm.deviceGroup(QStringLiteral("设备A")) == QStringLiteral("华东"));

    // Grouped view: groups order kept, __ungrouped__ last and only if needed.
    const QStringList all = {QStringLiteral("设备A"), QStringLiteral("设备B"),
                             QStringLiteral("设备C"), QStringLiteral("设备D")};
    QList<GroupedDevices> view = gm.groupedDevices(all);
    CHECK(view.size() == 3);
    CHECK(view[0].group == QStringLiteral("华东") && view[0].devices.size() == 2);
    CHECK(view[1].group == QStringLiteral("华南地区") && view[1].devices == QStringList{QStringLiteral("设备B")});
    CHECK(view[2].group == GroupManager::kUngrouped && view[2].devices == QStringList{QStringLiteral("设备D")});

    // No ungrouped devices -> no __ungrouped__ entry; empty groups still listed.
    view = gm.groupedDevices({QStringLiteral("设备A")});
    CHECK(view.size() == 2);
    CHECK(view[1].devices.isEmpty());

    // Device rename / delete hooks.
    gm.onDeviceRenamed(QStringLiteral("设备A"), QStringLiteral("设备A2"));
    CHECK(gm.deviceGroup(QStringLiteral("设备A")).isEmpty());
    CHECK(gm.deviceGroup(QStringLiteral("设备A2")) == QStringLiteral("华东"));
    gm.onDeviceDeleted(QStringLiteral("设备A2"));
    CHECK(gm.deviceGroup(QStringLiteral("设备A2")).isEmpty());

    // DELETE group drops its device mappings (devices become ungrouped).
    gm.deleteGroup(QStringLiteral("华南地区"));
    GroupData after = gm.loadGroups();
    CHECK(after.groups == QStringList{QStringLiteral("华东")});
    CHECK(!after.deviceGroupMap.contains(QStringLiteral("设备B")));

    // Round-trip through save/load preserves everything.
    GroupData rt;
    rt.groups = {QStringLiteral("g1"), QStringLiteral("g2")};
    rt.deviceGroupMap.insert(QStringLiteral("d1"), QStringLiteral("g1"));
    CHECK(gm.saveGroups(rt));
    const GroupData back = gm.loadGroups();
    CHECK(back.groups == rt.groups);
    CHECK(back.deviceGroupMap == rt.deviceGroupMap);

    QFile::remove(path);
}

// 多级分组：路径语义、同级查重（不同父分组允许同名）、旧格式迁移与
// 删除/改名/移动语义（含上移合并）。
static void testGroupHierarchy()
{
    const QString path = QDir::temp().filePath(QStringLiteral("cubeshell_groups_hier_test.json"));
    QFile::remove(path);
    GroupManager gm(path);

    // CREATE：子分组的父必须真实存在；同级重名被拒；名字非法被拒。
    CHECK(gm.createGroup(QStringLiteral("华东")));
    CHECK(gm.createGroup(QStringLiteral("上海"), QStringLiteral("华东")));
    CHECK(gm.createGroup(QStringLiteral("浦东"), QStringLiteral("华东/上海")));
    CHECK(!gm.createGroup(QStringLiteral("苏州"), QStringLiteral("不存在")));
    CHECK(!gm.createGroup(QStringLiteral("上海"), QStringLiteral("华东")));   // 同级同名
    CHECK(!gm.createGroup(QStringLiteral("浦东"), QStringLiteral("华东/上海"))); // 同级同名
    CHECK(!gm.createGroup(QStringLiteral("a/b")));                            // 名字含 /
    CHECK(!gm.createGroup(GroupManager::kUngrouped));                         // 保留名

    // 核心修复语义：不同父分组下允许同名子分组（含顶层）。
    CHECK(gm.createGroup(QStringLiteral("浦东"), QStringLiteral("华东")));    // 华东/浦东
    CHECK(gm.createGroup(QStringLiteral("华南")));
    CHECK(gm.createGroup(QStringLiteral("浦东"), QStringLiteral("华南")));    // 华南/浦东
    CHECK(gm.createGroup(QStringLiteral("浦东")));                            // 顶层浦东
    CHECK(!gm.createGroup(QStringLiteral("浦东"), QStringLiteral("华东")));   // 已有 → 拒

    // On-disk shape：层级由 groups 里的路径表达，不再写 group_parents 键。
    {
        QFile f(path);
        CHECK(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        CHECK(!root.contains(QStringLiteral("group_parents")));
        const QJsonArray groups = root.value(QStringLiteral("groups")).toArray();
        CHECK(groups.contains(QJsonValue(QStringLiteral("华东/上海/浦东"))));
        CHECK(groups.contains(QJsonValue(QStringLiteral("华南/浦东"))));
        CHECK(groups.contains(QJsonValue(QStringLiteral("浦东"))));
    }

    // effectiveParent：路径末段之前的段落；顶层返回空串。
    GroupData data = gm.loadGroups();
    CHECK(GroupManager::effectiveParent(data, QStringLiteral("华东/上海/浦东"))
              == QStringLiteral("华东/上海"));
    CHECK(GroupManager::effectiveParent(data, QStringLiteral("华东")).isEmpty());
    CHECK(GroupManager::effectiveParent(data, QStringLiteral("不存在")).isEmpty());

    // 同名分组各自挂对设备：groupedDevices 按路径分桶，互不串桶。
    gm.moveDeviceToGroup(QStringLiteral("设备甲"), QStringLiteral("华东"));
    gm.moveDeviceToGroup(QStringLiteral("设备乙"), QStringLiteral("华东/上海/浦东"));
    gm.moveDeviceToGroup(QStringLiteral("设备丙"), QStringLiteral("华东/浦东"));
    gm.moveDeviceToGroup(QStringLiteral("设备丁"), QStringLiteral("华南/浦东"));
    const QStringList all = {QStringLiteral("设备甲"), QStringLiteral("设备乙"),
                             QStringLiteral("设备丙"), QStringLiteral("设备丁")};
    QList<GroupedDevices> view = gm.groupedDevices(all);
    CHECK(view.size() == 7);
    CHECK(view[0].group == QStringLiteral("华东")
              && view[0].devices == QStringList{QStringLiteral("设备甲")});
    CHECK(view[2].group == QStringLiteral("华东/上海/浦东")
              && view[2].devices == QStringList{QStringLiteral("设备乙")});
    CHECK(view[3].group == QStringLiteral("华东/浦东")
              && view[3].devices == QStringList{QStringLiteral("设备丙")});
    CHECK(view[5].group == QStringLiteral("华南/浦东")
              && view[5].devices == QStringList{QStringLiteral("设备丁")});
    CHECK(view[6].group == QStringLiteral("浦东") && view[6].devices.isEmpty());

    // setGroupParent：目的地同级同名冲突、自己、后代、未知父都被拒。
    CHECK(!gm.setGroupParent(QStringLiteral("华东/上海/浦东"), QString()));   // 顶层已有"浦东"
    CHECK(!gm.setGroupParent(QStringLiteral("华东/浦东"), QStringLiteral("华东/上海"))); // 上海下已有"浦东"
    CHECK(!gm.setGroupParent(QStringLiteral("华东/上海/浦东"), QStringLiteral("华东/上海/浦东"))); // 自己
    CHECK(!gm.setGroupParent(QStringLiteral("华东/上海"), QStringLiteral("华东/上海/浦东")));     // 自己的后代
    CHECK(!gm.setGroupParent(QStringLiteral("华东/上海/浦东"), QStringLiteral("不存在")));
    // 挂到孪生兄弟下面是合法的：华东/浦东 → 华东/上海/浦东/浦东。
    CHECK(gm.setGroupParent(QStringLiteral("华东/浦东"), QStringLiteral("华东/上海/浦东")));
    data = gm.loadGroups();
    CHECK(data.groups.contains(QStringLiteral("华东/上海/浦东/浦东")));
    CHECK(GroupManager::descendants(data, QStringLiteral("华东/上海/浦东")).size() == 1);
    // 挪回去。
    CHECK(gm.setGroupParent(QStringLiteral("华东/上海/浦东/浦东"), QStringLiteral("华东")));
    data = gm.loadGroups();
    CHECK(data.groups.contains(QStringLiteral("华东/浦东")));
    CHECK(GroupManager::descendants(data, QStringLiteral("华东")).size() == 3); // 上海、上海/浦东、浦东

    // RENAME：同级重名被拒；孪生改名后歧义消失，文件自愈回裸名（最短无歧义形）。
    CHECK(gm.renameGroup(QStringLiteral("华东/浦东"), QStringLiteral("苏州")));
    CHECK(!gm.renameGroup(QStringLiteral("华东/苏州"), QStringLiteral("上海"))); // 华东下已有"上海"
    CHECK(!gm.renameGroup(QStringLiteral("不存在"), QStringLiteral("x")));
    CHECK(!gm.renameGroup(QStringLiteral("华东/苏州"), QStringLiteral("x/y")));  // 名字含 /
    data = gm.loadGroups();
    CHECK(data.groups.contains(QStringLiteral("华东/苏州")));
    CHECK(!data.groups.contains(QStringLiteral("华东/浦东")));
    CHECK(data.deviceGroupMap.value(QStringLiteral("设备丙")) == QStringLiteral("华东/苏州"));
    {
        QFile f(path);
        CHECK(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        // "苏州"叶子名现在全局唯一 → device 值写裸名（Python 端可正常显示）。
        CHECK(root.value(QStringLiteral("device_group_map")).toObject()
                  .value(QStringLiteral("设备丙")).toString() == QStringLiteral("苏州"));
    }

    // DELETE 中间分组：子分组上移一级挂到被删分组的父级。
    gm.moveDeviceToGroup(QStringLiteral("设备甲"), QStringLiteral("华东/上海"));
    gm.deleteGroup(QStringLiteral("华东/上海"));
    data = gm.loadGroups();
    CHECK(!data.groups.contains(QStringLiteral("华东/上海")));
    // 浦东（原华东/上海/浦东）上移 = 父(华东) + 叶子名(浦东)；华东下原本的
    // "浦东"已改名"苏州"，目标位置空闲 → 正常上移，不合并。
    CHECK(data.groups.contains(QStringLiteral("华东/浦东")));
    CHECK(!data.groups.contains(QStringLiteral("华东/上海/浦东")));
    // 组内设备：设备甲（挂华东/上海）移到被删分组的父级 华东。
    CHECK(data.deviceGroupMap.value(QStringLiteral("设备甲")) == QStringLiteral("华东"));

    // 上移合并用例：华东下建"新区"，新区下建"苏州"（叶子名与华东/苏州相同），
    // 删掉新区 → 新区/苏州 上移为 华东/苏州 → 已存在 → 并入幸存者。
    CHECK(gm.createGroup(QStringLiteral("新区"), QStringLiteral("华东")));
    CHECK(gm.createGroup(QStringLiteral("苏州"), QStringLiteral("华东/新区")));
    gm.deleteGroup(QStringLiteral("华东/新区"));
    data = gm.loadGroups();
    CHECK(!data.groups.contains(QStringLiteral("华东/新区")));
    CHECK(!data.groups.contains(QStringLiteral("华东/新区/苏州")));
    CHECK(data.groups.count(QStringLiteral("华东/苏州")) == 1); // 合并，没有重复节点

    // DELETE 顶层分组：无上级 → 组内设备回落未分组（删映射）；
    // 子分组上移为顶层时撞上现存顶层同名节点 → 并入。
    gm.moveDeviceToGroup(QStringLiteral("设备甲"), QStringLiteral("华东/浦东"));
    gm.deleteGroup(QStringLiteral("华东"));
    data = gm.loadGroups();
    CHECK(!data.groups.contains(QStringLiteral("华东")));
    CHECK(!data.groups.contains(QStringLiteral("华东/浦东"))); // 并入顶层"浦东"
    CHECK(data.groups.count(QStringLiteral("浦东")) == 1);
    CHECK(data.deviceGroupMap.contains(QStringLiteral("设备甲")) == false);
    CHECK(data.deviceGroupMap.value(QStringLiteral("设备甲")).isEmpty());
    CHECK(data.deviceGroupMap.value(QStringLiteral("设备丙")) == QStringLiteral("苏州")); // 随上移到顶层

    // Round-trip：路径形态原样保存/读回。
    GroupData rt;
    rt.groups = {QStringLiteral("g1"), QStringLiteral("g1/g2")};
    rt.deviceGroupMap.insert(QStringLiteral("d1"), QStringLiteral("g1/g2"));
    CHECK(gm.saveGroups(rt));
    const GroupData back = gm.loadGroups();
    CHECK(back.groups == rt.groups);
    CHECK(back.deviceGroupMap == rt.deviceGroupMap);

    // 旧格式迁移（Python 侧写出的形状）：裸名字 + group_parents。
    // 正常链：名字沿父链拼成完整路径；设备裸值解析为路径。
    {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QByteArrayLiteral(
            "{\"groups\":[\"华东\",\"上海\",\"浦东\"],"
            "\"device_group_map\":{\"设备甲\":\"浦东\"},"
            "\"group_parents\":{\"上海\":\"华东\",\"浦东\":\"上海\"}}"));
    }
    data = gm.loadGroups();
    CHECK(data.groups == (QStringList{QStringLiteral("华东"), QStringLiteral("华东/上海"),
                                      QStringLiteral("华东/上海/浦东")}));
    CHECK(data.deviceGroupMap.value(QStringLiteral("设备甲")) == QStringLiteral("华东/上海/浦东"));

    // 旧格式脏数据：成环（甲↔乙）与父未知（丙）都按顶层修复，节点不消失。
    {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QByteArrayLiteral(
            "{\"groups\":[\"甲\",\"乙\",\"丙\"],\"device_group_map\":{\"设备丙\":\"丙\"},"
            "\"group_parents\":{\"乙\":\"甲\",\"甲\":\"乙\",\"丙\":\"不存在\"}}"));
    }
    data = gm.loadGroups();
    CHECK(data.groups.size() == 3);
    CHECK(GroupManager::effectiveParent(data, QStringLiteral("甲")).isEmpty());
    CHECK(GroupManager::effectiveParent(data, QStringLiteral("乙")).isEmpty());
    CHECK(GroupManager::effectiveParent(data, QStringLiteral("丙")).isEmpty());
    CHECK(data.deviceGroupMap.value(QStringLiteral("设备丙")) == QStringLiteral("丙"));

    // 歧义设备引用：同名叶子存在多个时，裸值无法解析 → 条目读入即丢弃（H3，
    // 设备视为未分组，下次保存文件自愈）。
    {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QByteArrayLiteral(
            "{\"groups\":[\"华东\",\"华东/上海\",\"华东/上海/浦东\",\"华东/浦东\",\"浦东\"],"
            "\"device_group_map\":{\"设备丁\":\"浦东\",\"设备戊\":\"华东/浦东\"}}"));
    }
    data = gm.loadGroups();
    CHECK(!data.deviceGroupMap.contains(QStringLiteral("设备丁")));  // 歧义 → 丢
    CHECK(data.deviceGroupMap.value(QStringLiteral("设备戊")) == QStringLiteral("华东/浦东")); // 精确路径保留

    // 脏文件：同一路径出现两次 → 去重（H1）。
    {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QByteArrayLiteral(
            "{\"groups\":[\"a\",\"a\",\"x/y\",\"x/y\"],\"device_group_map\":{}}"));
    }
    data = gm.loadGroups();
    CHECK(data.groups == (QStringList{QStringLiteral("a"), QStringLiteral("x/y")}));

    // 平铺数据写回：不带 group_parents 键，与 Python 格式逐字节兼容。
    GroupData flat;
    flat.groups = {QStringLiteral("only")};
    CHECK(gm.saveGroups(flat));
    {
        QFile f(path);
        CHECK(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        CHECK(!root.contains(QStringLiteral("group_parents")));
        CHECK(root.value(QStringLiteral("groups")).toArray().size() == 1);
    }

    QFile::remove(path);
}

// conf/linux_commands.json：树形结构/过滤/读写往返。
static void testFrequentlyUsedCommands()
{
    FrequentlyUsedCommands cmds;
    QString err;
    CHECK(cmds.load(confDir() + QStringLiteral("/linux_commands.json"), &err));
    CHECK(err.isEmpty());
    CHECK(!cmds.isEmpty());

    // The real data has a category row with children ("文件管理" -> cat...).
    // 空树时 first() 是未定义行为，先记一次失败再退出，别把整个用例带崩。
    if (cmds.isEmpty()) {
        qWarning() << "FAIL: conf/linux_commands.json 不可读，后续断言跳过";
        ++failures;
        return;
    }
    CHECK(cmds.entries().first().hasChildren());
    const CommandEntry *cat = cmds.find(QStringLiteral("cat"));
    CHECK(cat != nullptr);
    CHECK(cat && !cat->description.isEmpty());

    // Recursive case-insensitive filtering keeps matching subtrees.
    const QList<CommandEntry> hits = cmds.filter(QStringLiteral("CHOWN"));
    CHECK(!hits.isEmpty());
    bool found = false;
    for (const CommandEntry &top : hits) {
        for (const CommandEntry &child : top.children)
            if (child.command == QStringLiteral("chown"))
                found = true;
    }
    CHECK(found);
    // Empty needle returns the full tree.
    CHECK(cmds.filter(QString()).size() == cmds.entries().size());

    // Write/read round-trip in the Python-compatible {"treeData": ...} shape.
    const QString tmp = QDir::temp().filePath(QStringLiteral("cubeshell_cmds_test.json"));
    CHECK(cmds.save(tmp, &err));
    FrequentlyUsedCommands reloaded;
    CHECK(reloaded.load(tmp, &err));
    CHECK(reloaded.entries().size() == cmds.entries().size());
    const CommandEntry *cat2 = reloaded.find(QStringLiteral("cat"));
    CHECK(cat2 && cat2->option == cat->option && cat2->description == cat->description);
    {
        QFile f(tmp);
        CHECK(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        CHECK(root.value(QStringLiteral("treeData")).isArray());
    }
    QFile::remove(tmp);
}

// 打包产物（macOS .app / Windows 安装包 / 鸿蒙 HAP）里没有 conf/ 目录，
// 「Linux常用命令查找」曾因此只显示「未找到 linux_commands.json」。命令库现在
// 编进二进制（conf/conf.qrc），这里校验内置副本确实在、且能解析出真实数据。
static void testEmbeddedCommandsResource()
{
    FrequentlyUsedCommands embedded;
    QString err;
    CHECK(embedded.load(QStringLiteral(":/conf/linux_commands.json"), &err));
    CHECK(err.isEmpty());
    CHECK(!embedded.isEmpty());
    CHECK(embedded.find(QStringLiteral("cat")) != nullptr);

    // 探测兜底到 qrc，永不返回空路径 —— 对话框因此不会再落到失败分支。
    const QString resolved = FrequentlyUsedCommands::defaultPath();
    CHECK(!resolved.isEmpty());
    FrequentlyUsedCommands byDefault;
    CHECK(byDefault.load(resolved, &err));
    CHECK(!byDefault.isEmpty());
}

static void testConfigUtil()
{
    QString err;

    // JSON: real theme.json sample.
    const QJsonValue theme = ConfigUtil::readJson(confDir() + QStringLiteral("/theme.json"), &err);
    CHECK(theme.isObject());
    CHECK(theme.toObject().value(QStringLiteral("appearance")).toString() == QStringLiteral("dark"));

    // JSON write + reload (indent=4 equivalent).
    const QString tmp = QDir::temp().filePath(QStringLiteral("cubeshell_cfg_test.json"));
    QJsonObject obj;
    obj.insert(QStringLiteral("键"), QStringLiteral("值")); // 非 ASCII 需原样保留
    obj.insert(QStringLiteral("n"), 42);
    CHECK(ConfigUtil::writeJson(tmp, obj, &err));
    const QJsonValue back = ConfigUtil::readJson(tmp, &err);
    CHECK(back.toObject() == obj);
    QFile::remove(tmp);

    // TOML: real frpc.toml sample (scalars, dotted key, [[proxies]]).
    const QVariantMap frpc = ConfigUtil::readToml(confDir() + QStringLiteral("/frpc.toml"), &err);
    CHECK(err.isEmpty());
    CHECK(frpc.value(QStringLiteral("serverAddr")).toString() == QStringLiteral("127.0.0.1"));
    CHECK(frpc.value(QStringLiteral("serverPort")).toLongLong() == 7000);
    CHECK(frpc.value(QStringLiteral("auth")).toMap()
              .value(QStringLiteral("token")).toString() == QStringLiteral("123456"));
    const QVariantList proxies = frpc.value(QStringLiteral("proxies")).toList();
    CHECK(proxies.size() == 1);
    const QVariantMap p0 = proxies.first().toMap();
    CHECK(p0.value(QStringLiteral("name")).toString() == QStringLiteral("ssh_01"));
    CHECK(p0.value(QStringLiteral("type")).toString() == QStringLiteral("tcp"));
    CHECK(p0.value(QStringLiteral("localPort")).toLongLong() == 8080);
    CHECK(p0.value(QStringLiteral("remotePort")).toLongLong() == 80);

    // Parser corner cases: comments, bools, floats, inline arrays, [table].
    const QVariantMap t = ConfigUtil::parseToml(QStringLiteral(
        "a = 1 # comment\n"
        "b = \"x # not a comment\"\n"
        "ok = true\n"
        "pi = 3.14\n"
        "list = [1, 2, 3]\n"
        "[sec]\n"
        "k = 'lit'\n"), &err);
    CHECK(err.isEmpty());
    CHECK(t.value(QStringLiteral("a")).toLongLong() == 1);
    CHECK(t.value(QStringLiteral("b")).toString() == QStringLiteral("x # not a comment"));
    CHECK(t.value(QStringLiteral("ok")).toBool());
    CHECK(qAbs(t.value(QStringLiteral("pi")).toDouble() - 3.14) < 1e-9);
    CHECK(t.value(QStringLiteral("list")).toList().size() == 3);
    CHECK(t.value(QStringLiteral("sec")).toMap().value(QStringLiteral("k")).toString() == QStringLiteral("lit"));

    // Extension routing.
    CHECK(ConfigUtil::loadConfig(confDir() + QStringLiteral("/theme.json")).isValid());
    CHECK(ConfigUtil::loadConfig(confDir() + QStringLiteral("/frpc.toml")).isValid());
    CHECK(!ConfigUtil::loadConfig(QStringLiteral("/no/such/file.ini")).isValid());
}

static void testGlobalState()
{
    // vars constants survive the port.
    CHECK(QString::fromLatin1(vars::CONF_FILE) == QStringLiteral("tunnel.json"));
    CHECK(QString::fromLatin1(vars::keys::SSH_ADDRESS) == QStringLiteral("ssh_address"));
    CHECK(QString::fromLatin1(vars::cmds::SSH_KILL_NIX) == QStringLiteral("pkill ssh"));

    // Paths must land where Python's appdirs puts them (config interop!).
    const QString cfg = GlobalState::configDir();
    CHECK(cfg.endsWith(QStringLiteral("/cube-shell")));
#ifdef Q_OS_MACOS
    CHECK(cfg.contains(QStringLiteral("Library/Application Support")));
    CHECK(GlobalState::dataDir() == cfg); // macOS: config == data
#endif
    CHECK(QDir(cfg).exists()); // created on demand, like os.makedirs
    CHECK(GlobalState::configFilePath(QStringLiteral("config.dat"))
          == cfg + QStringLiteral("/config.dat"));
    CHECK(GlobalState::tunnelConfigPath().endsWith(QStringLiteral("cube-shell/tunnel.json")));
    CHECK(GlobalState::groupsConfigPath().endsWith(QStringLiteral("cube-shell/groups.json")));

    // Theme state from the real conf/theme.json.
    GlobalState &gs = GlobalState::instance();
    CHECK(&gs == &GlobalState::instance()); // singleton
    QString err;
    CHECK(gs.loadTheme(confDir() + QStringLiteral("/theme.json"), &err));
    CHECK(gs.appearance() == QStringLiteral("dark"));
    CHECK(gs.language() == QStringLiteral("zh_CN"));
    CHECK(gs.fontSize() == 14);
    CHECK(!gs.fontFamily().isEmpty());

    // In-memory mutation only (do NOT saveTheme(): conf/theme.json is real data).
    gs.setAppearance(QStringLiteral("Light"));
    CHECK(gs.appearance() == QStringLiteral("light"));
    gs.setFont(QStringLiteral("Menlo"), 16);
    CHECK(gs.fontFamily() == QStringLiteral("Menlo") && gs.fontSize() == 16);

    // 命令补全开关：theme.json 里没有该键时缺省 true（与加开关之前的行为一致），
    // 显式关闭后能读回 false。
    CHECK(gs.commandCompletionEnabled());
    gs.setCommandCompletionEnabled(false);
    CHECK(!gs.commandCompletionEnabled());
    gs.setCommandCompletionEnabled(true);
    CHECK(gs.commandCompletionEnabled());
}

static void testSecretsCompileLevel()
{
    // Compile/link-level only: take the addresses so the symbols must exist,
    // but never touch the real Keychain from a unit test.
    auto storeFn = static_cast<bool (*)(const QString &, const QString &,
                                        const QString &, QString *)>(&Secrets::storeSecret);
    auto getFn = static_cast<QString (*)(const QString &, const QString &,
                                         QString *)>(&Secrets::retrieveSecret);
    auto delFn = static_cast<bool (*)(const QString &, const QString &,
                                      QString *)>(&Secrets::deleteSecret);
    auto keyFn = static_cast<QString (*)(const QString &)>(&Secrets::aiApiKey);
    CHECK(storeFn != nullptr && getFn != nullptr && delFn != nullptr && keyFn != nullptr);
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testGroupManager();
    testGroupHierarchy();
    testEmbeddedCommandsResource();
    testFrequentlyUsedCommands();
    testConfigUtil();
    testGlobalState();
    testSecretsCompileLevel();
    qInfo() << (failures == 0 ? "ALL PASS" : "FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
