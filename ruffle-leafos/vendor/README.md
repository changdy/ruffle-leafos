# 固定第三方参考

- `jawaka/` 仅保留本项目参考和源码打包所需的七个文件：MIT 许可、游戏菜单、Tabs 主题及设置/主题解析源码；固定提交与 SHA-256 见 [jawaka.lock.json](jawaka.lock.json)。这些文件用于理解和追溯菜单实现，不单独构建 Jawaka。其余上游源码、工具、主题图和其他平台资源已移出源码树，本地备份在工作区 `work/repository-cleanup-20261025/unused-reference/jawaka/`。
- `leaf-contracts/` 保留固定官方契约的 schema、校验脚本、说明和测试夹具，打包会调用 CONTENT-1 校验器。固定提交见 [references.lock.json](../adapter/references.lock.json)。夹具中的 `.so`、`.png` 和 `.zip` 是契约测试输入，按用途保留。

仓库的依赖边界和构建方式见[工程实现细节](../docs/implementation.md)。
