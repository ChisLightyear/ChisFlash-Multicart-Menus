# ChisFlash Multicart Menus

ChisFlash GBA 合卡菜单合集，统一维护 **LK 菜单、LK 水线菜单、NS 菜单和精简 NS 菜单**。本仓库提供源码、必要资源与构建文件，编译成品在 [Releases](https://github.com/ChisLightyear/ChisFlash-Multicart-Menus/releases) 下载。

## 鸣谢

特别感谢 **[Lesserkuma](https://github.com/Lesserkuma)** 创作的 **[GBA Multi Game Menu](https://github.com/Lesserkuma/GBA_MultiMenu)**，它是本项目所基于的原始 LK 菜单。

感谢 **[ZaindORp / orzgithub](https://github.com/orzgithub)** 对 **[GBA_MultiMenu_extended](https://github.com/orzgithub/GBA_MultiMenu_extended)** 的扩展与维护。

本项目在上述工作的基础上进行 ChisFlash 适配、界面扩展及兼容性修复，保留原作者署名和许可证。感谢原作者及相关开源贡献者的付出。

## 四套菜单

| 目录 | 菜单 | 说明 | Release 文件 |
|---|---|---|---|
| `LK/` | LK 菜单 | 经典列表界面，普通合卡布局 | `lk_multimenu.gba` |
| `LK-WL/` | LK 水线菜单 | 经典列表界面，支持配套硬件的碎片映射布局 | `wl_magic_menu.gba` |
| `NS/` | NS 菜单 | 完整 NS 界面，提供封面、设置及容量显示等功能 | `universal_menu.gba` |
| `NS-Lite/` | 精简 NS 菜单 | 精简 NS 界面 | `simplified_ns_menu.gba` |

请根据卡带硬件及现有合卡布局选择菜单。碎片映射需要硬件和工具共同支持。

## 下载与使用

从 [Releases](https://github.com/ChisLightyear/ChisFlash-Multicart-Menus/releases) 下载单个菜单或四款合包，通过匹配的 ChisFlash 上位机或合卡器导入、更新菜单。

菜单文件不包含游戏。更新已有卡带时，请使用工具的菜单更新功能，不要把裸菜单文件当作完整合卡镜像整卡覆盖。

## 编译

准备 devkitPro、devkitARM 和 libgba，在配置好工具链的终端中运行：

```sh
make
```

也可以单独编译，例如：

```sh
make NS
```

输出位于 `.build/` 下对应菜单目录。

## 启动兼容性修复

`v2026.11.09` 统一修正四套菜单交接游戏前的中断状态，避免部分游戏在初始化完成前提前进入中断，造成黑屏或异常启动。马里奥赛车汉化版跳过开场图后的黑屏是本次修复案例。

四套菜单已通过 01G Verilog 功能仿真与 mGBA、真实 GBA BIOS 联调，能够进入马里奥赛车赛道；另已验证普通、WL 和通用映射启动组合。本版尚未完成实卡验证，详细验证范围见 Release。

## 许可证

菜单代码沿用 **GNU GPL v3**，见 [LICENSE](LICENSE) 及各菜单目录中的许可证。字体等资源保留各自的许可证和署名。
