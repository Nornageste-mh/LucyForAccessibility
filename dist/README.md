# dist —— 构建产物目录

这个目录只放**构建出来的东西**，不放源码：

- `d3dx9_40.dll`：补丁的代理 DLL，由 `build\build.cmd` 生成后复制到这里；
- `LucyForAccessibility-<版本号>.zip`：发布包，由 `build\release.ps1` 生成。

约定：

- 本目录的产物**全部不入库**（见 `.gitignore` 的 `dist/*`，只放行本文件）。
- 发布包里的文件必须能从本仓库源码重新构建出来（Framework G45）。
- **禁止**把游戏文件（`audiere.dll`、原版 `d3dx9_40.dll`、游戏资源等）放进这里。
- 打包必须用 `build\makezip.ps1`（禁止 `Compress-Archive`，避免反斜杠条目名）。
