# LILYGO Maps 架构总览

本应用是基于 LILYGO UI AppKit 和 LVGL 9.5 的在线地图浏览器。运行目标为
CM0 的 ARM64 Linux 环境，同时保留 SDL2 主机模拟器用于开发和测试。

项目介绍见 [README](../README.md)，依赖安装、编译和打包见
[README 构建与测试章节](../README.md#build-and-test)，在线服务与缓存配置见
[服务与缓存说明](02-services-and-cache.md)。

## 数据流

```text
启动 / 尚无定位 ──> IP 定位 ──> MapModel 中心点 ──┐
返回 IP 位置 ─────> 本次定位缓存 ────────────────┤
地址输入 ─────────> Nominatim ─> MapModel 中心点 ──┼─> 可见瓦片集合
拖动 / 缩放 ───────────────────> MapModel ─────────┘        │
                                                             v
LVGL 图层 <── 主线程事件队列 <── 磁盘缓存 <── HTTPS 瓦片下载
```

`MapService` 的两个工作线程只处理网络、JSON 和文件系统。所有 LVGL 对象都由主线程
创建和修改，页面通过 80 ms 定时器轮询服务结果。页面销毁时先删除定时器和 LVGL
对象，再销毁 ViewModel；服务析构会取消正在运行的 curl 子进程并等待工作线程退出。

右上角缩放控件下方的定位按钮返回本次运行中最近成功的 IP 坐标和地区标签，缩放恢复为 11；
没有缓存时重新查询 IP。拖动期间收到的定位结果只更新缓存，不打断浏览；返回 IP 后，
此前尚未完成的地址搜索结果不会再次改变地图中心。

## 模块边界

| 模块 | 责任 |
| --- | --- |
| `domain/map_model.*` | Web Mercator 投影、经度环绕、纬度限制、平移缩放和可见瓦片。 |
| `domain/tile_cache.*` | `openstreetmap/z/x/y.png` 路径、PNG 约束、原子写入、统计和清理。 |
| `services/http_client.*` | 无 shell 拼接的 curl 子进程、HTTPS 限制（固定 ip-api 定位地址除外）、超时、大小限制和取消。 |
| `services/map_service.*` | IP 定位、地址搜索、瓦片任务去重、视口代次和线程安全结果队列。 |
| `pages/map/map_view_model.*` | 页面状态、Subject 发布和用户命令。 |
| `pages/map/map_view.*` | 响应式 LVGL 控件、瓦片图层、手势、键盘与清理确认。 |

## 缓存一致性

- 下载内容必须是至多 1 MiB 的 `256 x 256` PNG，且具有正确的 PNG/IHDR/IEND 结构。
- 文件先写入同目录的 `.part`，完成后再 rename，应用不会读取半成品瓦片。
- 每次视口变化生成新的 `generation`，后台返回旧代次时只保留磁盘缓存，不更新 UI。
- 清缓存会增加 `cache_epoch` 并移除待处理任务；已在下载的旧任务不能再次写回。
- 递归清理只允许作用于名为 `openstreetmap` 的专用缓存根目录，环境变量误配时拒绝执行。
- root 设备默认使用 `/root/maps/openstreetmap`，与 K230 Launcher 离线瓦片约定兼容。

## 失败策略

- IP 定位依次尝试 ipapi.co、ipinfo.io 和 ip-api.com，请求失败、服务报错或坐标无效时切换下一接口；全部失败时显示世界地图，地图的拖动、缩放和地址搜索仍可使用。
- 仅最后备用的 `http://ip-api.com/json/` 允许未加密 HTTP，并禁止重定向；其他请求及其重定向仍仅允许 HTTPS。
- 单个瓦片失败时保留稳定的网格占位，不清除已经显示的其他瓦片。
- 地址没有结果或服务失败时保留当前地图中心，不跳到无效坐标。
- 缓存清理失败时保留已有数据显示，并在状态浮层呈现错误。

## 验收

主机配置、编译和测试：

```sh
cmake --preset host-simulator
cmake --build --preset host-simulator --parallel
ctest --preset host-simulator
```

UI 必须在 `568x1232` 和 `1232x568` 下验证，确保搜索栏、地图控件、状态、署名、
软键盘和确认框没有裁切或重叠。涉及打包和依赖变更时还必须完成 `cm0-cross` 构建。
