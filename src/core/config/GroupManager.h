#pragma once

// GroupManager.h — device group configuration management.
//
// 对应Python: core/group_manager.py
//
// 分组的内部标识是**完整路径**：显示名段以 "/" 连接（如 "华东/上海/浦东"），
// 顶层分组的路径就是裸名字。允许不同父分组下存在同名子分组（同级查重）。
//
// Persists to groups.json inside the user config dir. On-disk format:
//     {
//         "groups": ["华东地区", "华东地区/上海", "华东地区/上海/浦东", ...],
//         "device_group_map": { "设备A": "上海", "设备B": "华东地区/上海/浦东" }
//     }
// - "groups"：顶层条目 = 裸名字（平铺用法下与 Python 格式逐字节一致）；
//   嵌套分组条目天然含 "/"，层级完全由路径字符串表达，不再有 group_parents 键。
// - "device_group_map" 值按**最短无歧义**写：叶子名全局唯一时写裸名字（Python
//   端可正常显示）；仅叶子名存在同名节点时写完整路径（Python 端该设备回落
//   未分组——层级本就是 C++ 扩展，这是可接受的退化）。
// - 分组名不允许包含 "/"（创建/重命名时拒绝），也不允许叫 kUngrouped。
// Python 侧读时忽略未知键、写时丢弃：Python 改写文件后路径条目退化为平铺
// 字面量名字，层级丢失但不损坏；C++ 重新读入按平铺处理。
// 旧版文件（groups 为裸名字 + "group_parents" name->parent 键）在 loadGroups
// 时自动迁移为路径；load 不写盘，下次变更保存时自然落新格式。
// Like the Python module, every operation reads the file fresh and writes it
// back immediately, so both apps can share the file without a daemon.

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace cubeshell {

// In-memory mirror of the groups.json document.
// 对应Python: core/group_manager.py::_empty_data (结构定义)
struct GroupData {
    QStringList groups;                     // "groups": 每个节点的完整路径，顺序即展示顺序
    QHash<QString, QString> deviceGroupMap; // "device_group_map": device -> group path
};

// One entry of the grouped-devices view; order matters (groups order, then
// "__ungrouped__" last), so a list is used instead of a hash.
struct GroupedDevices {
    QString group;       // group path, or GroupManager::kUngrouped
    QStringList devices;
};

class GroupManager {
public:
    // Sentinel group "path" for devices without a group.
    // 对应Python: core/group_manager.py::get_grouped_devices 的 "__ungrouped__"
    static const QString kUngrouped;

    // filePath empty -> GlobalState::configDir()/groups.json (Python default).
    explicit GroupManager(const QString &filePath = QString());

    QString filePath() const { return m_filePath; }

    // ---- 路径助手（UI 建树/菜单也用） ----
    // 路径末段 = 显示名："华东/上海" -> "上海"；裸名字原样返回。
    static QString leafName(const QString &path);
    // 父路径："华东/上海" -> "华东"；顶层（裸名字）返回空串。
    static QString parentPath(const QString &path);
    // 分组名合法性：非空、不含 "/"、不等于 kUngrouped。
    static bool isValidGroupName(const QString &name);

    // 对应Python: core/group_manager.py::load_groups
    // Missing file or bad JSON yields the empty structure (never fails).
    // 旧版格式（裸名字 + group_parents）在此迁移为路径。
    GroupData loadGroups() const;

    // 对应Python: core/group_manager.py::save_groups
    bool saveGroups(const GroupData &data, QString *errorOut = nullptr) const;

    // 对应Python: core/group_manager.py::create_group
    // parentPath 非空时创建为该分组的子分组；parentPath 必须已存在。
    // 查重是**同级**的：不同父分组下允许同名（本修复的核心语义）。
    bool createGroup(const QString &name, const QString &parentPath = QString());

    // 多级分组：把分组挂到另一个父分组下（newParent 空串 = 移到顶层）。
    // 拒绝：目标不存在、挂到自己/自己的后代下、目的地已有同级同名分组。
    bool setGroupParent(const QString &path, const QString &newParent);

    // 分组的直接父路径；父节点不存在（不应发生）时返回空串。
    static QString effectiveParent(const GroupData &data, const QString &path);

    // 某分组的全部后代路径（不含自己），供"移动分组"时排除。
    static QStringList descendants(const GroupData &data, const QString &path);

    // 对应Python: core/group_manager.py::rename_group
    // path 为被改分组的完整路径；newName 是新的**显示名**（叶子名）。
    // 同级查重 + 名字合法性校验；全部后代路径与设备映射一并重写。
    bool renameGroup(const QString &path, const QString &newName);

    // 对应Python: core/group_manager.py::delete_group
    // 多级语义：子分组上移一级挂到被删分组的父级；组内设备移至上级分组
    // （无上级则回落到"未分组"，与平铺时代行为一致）。
    void deleteGroup(const QString &path);

    // 对应Python: core/group_manager.py::move_device_to_group
    // groupPath 是目标分组的完整路径。
    void moveDeviceToGroup(const QString &deviceName, const QString &groupPath);

    // 对应Python: core/group_manager.py::remove_device_from_group
    void removeDeviceFromGroup(const QString &deviceName);

    // 对应Python: core/group_manager.py::get_device_group (无分组返回空串)
    // 返回设备所在分组的完整路径。
    QString deviceGroup(const QString &deviceName) const;

    // 对应Python: core/group_manager.py::get_grouped_devices
    // Every known group is present (possibly empty); kUngrouped appears last
    // and only when there are ungrouped devices. group 字段是完整路径。
    QList<GroupedDevices> groupedDevices(const QStringList &allDeviceNames) const;

    // 对应Python: core/group_manager.py::on_device_deleted
    void onDeviceDeleted(const QString &deviceName);

    // 对应Python: core/group_manager.py::on_device_renamed
    void onDeviceRenamed(const QString &oldName, const QString &newName);

private:
    QString m_filePath;
};

} // namespace cubeshell
