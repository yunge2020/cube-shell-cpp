// GroupManager.cpp — device group persistence. See GroupManager.h.

#include "GroupManager.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

#include <algorithm>

#include "GlobalState.h"

namespace cubeshell {

const QString GroupManager::kUngrouped = QStringLiteral("__ungrouped__");

// ---- 路径助手 ----

// 路径末段 = 显示名："华东/上海" -> "上海"；裸名字原样返回。
QString GroupManager::leafName(const QString &path)
{
    const qsizetype idx = path.lastIndexOf(QLatin1Char('/'));
    return idx < 0 ? path : path.mid(idx + 1);
}

// 父路径："华东/上海" -> "华东"；顶层（裸名字）返回空串。
QString GroupManager::parentPath(const QString &path)
{
    const qsizetype idx = path.lastIndexOf(QLatin1Char('/'));
    return idx < 0 ? QString() : path.left(idx);
}

// 分组名合法性：非空、不含 "/"（路径分隔符）、不与未分组哨兵冲突。
bool GroupManager::isValidGroupName(const QString &name)
{
    return !name.isEmpty() && name != kUngrouped && !name.contains(QLatin1Char('/'));
}

// 对应Python: core/group_manager.py::_get_groups_file_path
GroupManager::GroupManager(const QString &filePath)
    : m_filePath(filePath.isEmpty() ? GlobalState::groupsConfigPath() : filePath)
{
}

// 对应Python: core/group_manager.py::load_groups
GroupData GroupManager::loadGroups() const
{
    GroupData data; // _empty_data(): {"groups": [], "device_group_map": {}}

    QFile f(m_filePath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return data;

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        return data; // json.JSONDecodeError -> empty structure

    const QJsonObject obj = doc.object();

    // Python tolerates missing / wrongly-typed keys — mirror that here.
    QStringList rawGroups;
    const QJsonValue groupsVal = obj.value(QStringLiteral("groups"));
    if (groupsVal.isArray()) {
        for (const QJsonValue &v : groupsVal.toArray()) {
            if (v.isString())
                rawGroups.append(v.toString());
        }
    }

    // 旧版多级格式键（裸名字 name -> 父名字）；新格式不再写入。
    QHash<QString, QString> legacyParents;
    const QJsonValue parentsVal = obj.value(QStringLiteral("group_parents"));
    if (parentsVal.isObject()) {
        const QJsonObject parentsObj = parentsVal.toObject();
        for (auto it = parentsObj.constBegin(); it != parentsObj.constEnd(); ++it)
            legacyParents.insert(it.key(), it.value().toString());
    }

    // 格式判定：groups 全为裸名字且带 group_parents 键 → 旧格式，迁移为路径；
    // 只要出现含 "/" 的条目即按新格式（路径）处理，group_parents 忽略。
    const bool legacy = !legacyParents.isEmpty()
        && std::none_of(rawGroups.cbegin(), rawGroups.cend(),
                        [](const QString &g) { return g.contains(QLatin1Char('/')); });

    if (legacy) {
        // 旧格式迁移：沿 name -> parent 图走到根拼出完整路径；
        // 成环 / 父级未知 / 自环的脏数据整条链按顶层处理（与旧 effectiveParent 修复语义一致）。
        for (const QString &name : rawGroups) {
            QString path = name;
            QSet<QString> seen;
            seen.insert(name);
            QString cur = legacyParents.value(name);
            bool dirty = false;
            while (!cur.isEmpty()) {
                if (seen.contains(cur) || !rawGroups.contains(cur)) {
                    dirty = true; // 成环或父级未知：按顶层
                    break;
                }
                seen.insert(cur);
                path.prepend(cur + QLatin1Char('/'));
                cur = legacyParents.value(cur);
            }
            if (dirty)
                path = name;
            if (!path.isEmpty() && !data.groups.contains(path))
                data.groups.append(path);
        }
    } else {
        for (const QString &g : rawGroups) {
            if (!g.isEmpty() && !data.groups.contains(g))
                data.groups.append(g);
        }
    }

    // device_group_map：裸值按唯一叶子名解析为路径（旧格式迁移与新格式短值共用
    // 一条规则）；含 "/" 的值按完整路径精确匹配。解析不到/歧义的条目直接丢弃
    // （设备视为未分组）——留在内存里只会被原样写回，永远无法自愈。
    const QJsonValue mapVal = obj.value(QStringLiteral("device_group_map"));
    if (mapVal.isObject()) {
        QHash<QString, QStringList> byLeaf;
        for (const QString &p : data.groups)
            byLeaf[leafName(p)].append(p);
        const QJsonObject mapObj = mapVal.toObject();
        for (auto it = mapObj.constBegin(); it != mapObj.constEnd(); ++it) {
            const QString ref = it.value().toString();
            if (ref.isEmpty())
                continue; // 本来就未分组，无需落盘
            if (!ref.contains(QLatin1Char('/'))) {
                const QStringList hits = byLeaf.value(ref);
                if (hits.size() == 1)
                    data.deviceGroupMap.insert(it.key(), hits.first());
                continue; // 唯一命中已写入；歧义/未知 → 丢弃
            }
            if (data.groups.contains(ref))
                data.deviceGroupMap.insert(it.key(), ref);
        }
    }
    return data;
}

// 对应Python: core/group_manager.py::save_groups (ensure_ascii=False, indent=2)
bool GroupManager::saveGroups(const GroupData &data, QString *errorOut) const
{
    // 层级完全由 "groups" 里的路径表达（顶层 = 裸名字），不再写 "group_parents"：
    // 平铺用法下文件与 Python 格式逐字节一致。

    // 叶子名出现次数，用于 device 值的"最短无歧义"改写。
    QHash<QString, int> leafCount;
    for (const QString &p : data.groups)
        leafCount[leafName(p)]++;

    QJsonArray groups;
    for (const QString &p : data.groups)
        groups.append(p);

    QJsonObject map;
    for (auto it = data.deviceGroupMap.constBegin();
         it != data.deviceGroupMap.constEnd(); ++it) {
        QString ref = it.value();
        // 已知路径且叶子名全局唯一 → 写裸名字（Python 端正常显示、既有文件字节不变）；
        // 歧义（存在同名节点）或无法解析的脏值 → 原样写完整路径/原值。
        if (data.groups.contains(ref) && leafCount.value(leafName(ref)) == 1)
            ref = leafName(ref);
        map.insert(it.key(), ref);
    }

    QJsonObject root;
    root.insert(QStringLiteral("groups"), groups);
    root.insert(QStringLiteral("device_group_map"), map);

    QSaveFile f(m_filePath);
    if (!f.open(QIODevice::WriteOnly)) {
        if (errorOut) *errorOut = QStringLiteral("cannot write %1").arg(m_filePath);
        return false;
    }
    // toJson emits UTF-8 without \u escapes — same as ensure_ascii=False.
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (errorOut) *errorOut = QStringLiteral("commit failed for %1").arg(m_filePath);
        return false;
    }
    return true;
}

// 对应Python: core/group_manager.py::create_group
bool GroupManager::createGroup(const QString &name, const QString &parentPath)
{
    if (!isValidGroupName(name))
        return false;
    GroupData data = loadGroups();
    // 同级查重：同一父分组下不允许同名；不同父分组下允许同名（本修复的核心语义）。
    const QString newPath =
        parentPath.isEmpty() ? name : parentPath + QLatin1Char('/') + name;
    if (data.groups.contains(newPath))
        return false;
    // 子分组：父分组必须真实存在（kUngrouped 不在 groups 里，天然被拒）。
    if (!parentPath.isEmpty() && !data.groups.contains(parentPath))
        return false;
    data.groups.append(newPath);
    saveGroups(data);
    return true;
}

bool GroupManager::setGroupParent(const QString &path, const QString &newParent)
{
    GroupData data = loadGroups();
    if (!data.groups.contains(path) || path == newParent)
        return false;
    if (!newParent.isEmpty() && !data.groups.contains(newParent))
        return false;
    // 不能挂到自己或自己的后代下面：后代 = 以 "path/" 为前缀（路径即层级，无环可成）。
    if (!newParent.isEmpty() && newParent.startsWith(path + QLatin1Char('/')))
        return false;
    // 目的地同级同名冲突：目标位置已被占用（含"挂回原父级"的自位置情形）。
    const QString newPath = newParent.isEmpty()
        ? leafName(path)
        : newParent + QLatin1Char('/') + leafName(path);
    if (data.groups.contains(newPath))
        return false;
    // 子树整体改前缀：被移动节点及其全部后代的路径跟着变。
    const QString oldPrefix = path + QLatin1Char('/');
    QStringList moved;
    moved.reserve(data.groups.size());
    for (const QString &p : data.groups) {
        if (p == path)
            moved.append(newPath);
        else if (p.startsWith(oldPrefix))
            moved.append(newPath + p.mid(oldPrefix.length()));
        else
            moved.append(p);
    }
    data.groups = moved;
    for (auto it = data.deviceGroupMap.begin(); it != data.deviceGroupMap.end(); ++it) {
        if (it.value() == path)
            it.value() = newPath;
        else if (it.value().startsWith(oldPrefix))
            it.value() = newPath + it.value().mid(oldPrefix.length());
    }
    saveGroups(data);
    return true;
}

// 分组的直接父路径；父节点不存在（磁盘脏数据）时按顶层（空串）处理。
QString GroupManager::effectiveParent(const GroupData &data, const QString &path)
{
    if (!data.groups.contains(path))
        return QString();
    const QString parent = parentPath(path);
    if (!parent.isEmpty() && !data.groups.contains(parent))
        return QString();
    return parent;
}

// 某分组的全部后代路径（不含自己），供"移动分组"时排除。
QStringList GroupManager::descendants(const GroupData &data, const QString &path)
{
    QStringList out;
    const QString prefix = path + QLatin1Char('/');
    for (const QString &p : data.groups) {
        if (p.startsWith(prefix))
            out.append(p);
    }
    return out;
}

// 对应Python: core/group_manager.py::rename_group
bool GroupManager::renameGroup(const QString &path, const QString &newName)
{
    if (!isValidGroupName(newName))
        return false;
    GroupData data = loadGroups();
    if (!data.groups.contains(path))
        return false;
    const QString parent = parentPath(path);
    const QString newPath =
        parent.isEmpty() ? newName : parent + QLatin1Char('/') + newName;
    // 同级查重（改回原名视为无效改名，与旧"改到已存在名字报错"行为一致）。
    if (newPath == path || data.groups.contains(newPath))
        return false;
    // 本节点与全部后代路径、引用这些路径的设备一并重写。
    const QString oldPrefix = path + QLatin1Char('/');
    for (int i = 0; i < data.groups.size(); ++i) {
        const QString &p = data.groups.at(i);
        if (p == path)
            data.groups[i] = newPath;
        else if (p.startsWith(oldPrefix))
            data.groups[i] = newPath + p.mid(oldPrefix.length());
    }
    for (auto it = data.deviceGroupMap.begin(); it != data.deviceGroupMap.end(); ++it) {
        if (it.value() == path)
            it.value() = newPath;
        else if (it.value().startsWith(oldPrefix))
            it.value() = newPath + it.value().mid(oldPrefix.length());
    }
    saveGroups(data);
    return true;
}

// 对应Python: core/group_manager.py::delete_group
void GroupManager::deleteGroup(const QString &path)
{
    GroupData data = loadGroups();
    if (!data.groups.contains(path))
        return;
    const QString parent = parentPath(path);
    const QString oldPrefix = path + QLatin1Char('/');
    // 子分组上移一级：挂到被删分组的父级；顶层分组被删则子分组升为顶层。
    // 上移后的路径若与现存节点重名（父分组下已有同名子分组），并入该节点。
    QStringList kept;
    kept.reserve(data.groups.size());
    QSet<QString> keptSet;
    for (const QString &p : data.groups) {
        if (p == path)
            continue;
        if (p.startsWith(oldPrefix)) {
            const QString tail = p.mid(oldPrefix.length());
            const QString np =
                parent.isEmpty() ? tail : parent + QLatin1Char('/') + tail;
            // 目的地已被占（既有节点或前面刚上移的兄弟）→ 合并进幸存者。
            if (keptSet.contains(np) || data.groups.contains(np))
                continue;
            keptSet.insert(np);
            kept.append(np);
        } else {
            keptSet.insert(p);
            kept.append(p);
        }
    }
    data.groups = kept;
    // 组内设备移到上级分组；没有上级则回落到"未分组"（删掉映射）。
    // 后代分组里的设备随子分组上移：同样的前缀改写规则（合并的并入幸存者）。
    for (auto it = data.deviceGroupMap.begin(); it != data.deviceGroupMap.end();) {
        const QString v = it.value();
        if (v == path) {
            if (parent.isEmpty() || !data.groups.contains(parent)) {
                it = data.deviceGroupMap.erase(it);
            } else {
                it.value() = parent;
                ++it;
            }
        } else if (v.startsWith(oldPrefix)) {
            const QString tail = v.mid(oldPrefix.length());
            it.value() = parent.isEmpty() ? tail : parent + QLatin1Char('/') + tail;
            ++it;
        } else {
            ++it;
        }
    }
    saveGroups(data);
}

// 对应Python: core/group_manager.py::move_device_to_group
void GroupManager::moveDeviceToGroup(const QString &deviceName, const QString &groupPath)
{
    // 不落脏值：空值与未分组哨兵都表示"无分组"（删映射即可），
    // 指向不存在分组的路径写了也只会静默回落未分组，不如直接拒绝。
    if (groupPath.isEmpty() || groupPath == kUngrouped)
        return;
    GroupData data = loadGroups();
    if (!data.groups.contains(groupPath))
        return;
    data.deviceGroupMap.insert(deviceName, groupPath);
    saveGroups(data);
}

// 对应Python: core/group_manager.py::remove_device_from_group
void GroupManager::removeDeviceFromGroup(const QString &deviceName)
{
    GroupData data = loadGroups();
    // QHash::remove 在 Qt6 返回 bool（Qt5 是 int）：直接当布尔判定，
    // 不要写 > 0——MSVC C4804 会对 bool 的 '>' 比较报警。
    if (data.deviceGroupMap.remove(deviceName))
        saveGroups(data);
}

// 对应Python: core/group_manager.py::get_device_group (无分组返回空串)
QString GroupManager::deviceGroup(const QString &deviceName) const
{
    return loadGroups().deviceGroupMap.value(deviceName);
}

// 对应Python: core/group_manager.py::get_grouped_devices
QList<GroupedDevices> GroupManager::groupedDevices(const QStringList &allDeviceNames) const
{
    const GroupData data = loadGroups();

    QList<GroupedDevices> result;
    result.reserve(data.groups.size() + 1);
    // Seed every known group (possibly empty), preserving order.
    for (const QString &path : data.groups)
        result.append({path, {}});

    QStringList ungrouped;
    for (const QString &deviceName : allDeviceNames) {
        const QString path = data.deviceGroupMap.value(deviceName);
        const int idx = path.isEmpty() ? -1 : int(data.groups.indexOf(path));
        if (idx >= 0)
            result[idx].devices.append(deviceName);
        else
            ungrouped.append(deviceName); // group unset, unknown, or ambiguous
    }

    // "__ungrouped__" appears only when there are ungrouped devices.
    if (!ungrouped.isEmpty())
        result.append({kUngrouped, ungrouped});
    return result;
}

// 对应Python: core/group_manager.py::on_device_deleted
void GroupManager::onDeviceDeleted(const QString &deviceName)
{
    removeDeviceFromGroup(deviceName);
}

// 对应Python: core/group_manager.py::on_device_renamed
void GroupManager::onDeviceRenamed(const QString &oldName, const QString &newName)
{
    GroupData data = loadGroups();
    auto it = data.deviceGroupMap.find(oldName);
    if (it != data.deviceGroupMap.end()) {
        const QString group = it.value();
        data.deviceGroupMap.erase(it);
        data.deviceGroupMap.insert(newName, group);
        saveGroups(data);
    }
}

} // namespace cubeshell
