# CloudZao 传输 / CloudZao transfer

CloudZao 是「文件管理」的直连接收功能，不是 OTA 来源，不替代 WiFi／热点浏览器传书或 USB。仅在绑定界面轮询授权，不后台收件，不走 4G，不替用户开关无线。未配置接入 ID 时隐藏入口。
CloudZao is a direct receive feature in Files, not an OTA source or a replacement for WiFi, hotspot or USB transfer. It polls only while pairing, never receives in the background, does not use cellular and does not change radio settings. Without an integration ID, its entry is hidden.

## 本机配置 / Local configuration

从[云造开发者文档](https://burner.cloudzao.com/zh/ota-docs)进入「我的固件」，复制固件卡片的接入 ID。不使用旧开发者密钥、官网中转或 `firmwareId`。仅在 Git 忽略的 `sdkconfig.cloudzao.local` 写入下面一行，将占位符换成自己的 ID；不要提交该文件或真实值。设备令牌不能填在这里。
From the [CloudZao developer documentation](https://burner.cloudzao.com/zh/ota-docs), open My Firmware and copy the integration ID from the firmware card. Do not use the legacy key, relay, or `firmwareId`. Put the following line only in the Git-ignored `sdkconfig.cloudzao.local` and replace the placeholder with your ID. Do not commit that file or the real value. Do not put device tokens here.

```ini
CONFIG_MOGO_CLOUDZAO_INTEGRATION_ID="YOUR_INTEGRATION_ID"
```

按[本机环境规则](../../ONBOARDING.md#本机环境硬约束--local-environment-hard-rules)激活指定环境，再用同一套正式代码构建。`build/cloudzao-local/sdkconfig` 是生成配置，会覆盖 defaults；换 ID 时应使用新的本机目录或明确更新该生成配置。
Activate the required environment in onboarding and build the same production code. `build/cloudzao-local/sdkconfig` is generated and overrides defaults; use a new local directory or explicitly update the generated configuration when changing IDs.

```sh
chmod 600 sdkconfig.cloudzao.local
mkdir -p build/cloudzao-local
IDF_TARGET=esp32s31 idf.py --preview -B build/cloudzao-local \
  -D SDKCONFIG="$PWD/build/cloudzao-local/sdkconfig" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;$PWD/sdkconfig.cloudzao.local" build
```

接入 ID 会编入镜像，使用本机配置构建的 BIN、ELF、生成配置和衍生产物不得公开。按[正式发布契约](RELEASE.md)构建公开制品时不加载本机配置且接入 ID 必须留空，因此公开制品不显示 CloudZao 入口。
The ID is embedded in the image. Do not publish binaries, ELF, generated configuration or derived artifacts from the local build. The public build specified by the release contract does not load the local overlay and requires an empty ID, so public images do not show the CloudZao entry.

## 设备操作 / On-device use

绑定、取令牌、拉清单和下载确认均直连 `https://claw.cloudzao.cn/xiaozhi/api/partner/v1`；不回退其他业务域名，不改变 mogo OTA 源。文件下载走清单中的链接，不附带设备令牌。
Pairing, token exchange and refresh, listing, and acknowledgements go directly to `https://claw.cloudzao.cn/xiaozhi/api/partner/v1`. There is no fallback to another API host and no change to the mogo OTA source. File downloads use the listed links without device tokens.

设备先下拉快捷栏打开 WiFi，确认已连接并校准时间。进入「文件管理 → CloudZao → 设置」，点「绑定 / 查看授权」，在 CloudZao App 或 CloudZao 小程序输入设备显示的六位码。页面只显示验证码；设备用 `deviceCode` 轮询，授权成功后可插入已挂载的 TF 卡收件。暂停、离开绑定页或锁屏不重新领码；回来点按钮可继续尚未过期的配对。已绑但缺少本机授权时，云造可能不返回新码；请在小程序检查设备状态，不以无码或 `409` 判为已授权。
Open Quick Settings to enable WiFi, confirm the connection, and set the device clock. Open Files → CloudZao → Settings, choose the binding action, and enter the displayed six-digit code in the CloudZao app or mini-program. The screen only shows the code; the device polls using `deviceCode`. A mounted TF card is required for receiving, not pairing. Pausing, leaving the binding view or locking does not issue another code; the button resumes a non-expired pairing. CloudZao can refuse to issue a code for an already-bound device with no local authorization. Check the mini-program; a missing code or `409` is not proof of authorization.

App 显示绑定成功不代表设备已领到授权；以设备显示「授权可用」为准。领取令牌途中超时，设备在当前绑定页保留六位码，等待至少服务端规定的轮询间隔再自动重试，不重新申请绑定码。会话失效后停止。`deviceCode` 只能成功兑换一次；若服务端已兑换但回包丢失，自动重试不一定能恢复，请先在 App 检查绑定状态。
An app-side binding does not mean the device has received authorization. Wait for the device's authorization confirmation. If token exchange times out, the pairing view keeps the six-digit code and retries after at least the server's polling interval without issuing a new code. Polling stops when the session expires. A `deviceCode` can be redeemed only once; if the server redeemed it but the reply was lost, retry may not recover it. Check binding in the app first.

待配对记录、设备令牌对及到期时间存本机独立 NVS 命名空间 `cloudzao`，不进 TF 设置备份、接收历史、请求日志或 BIN。读取清单／确认前按需刷新；访问令牌失效至多刷新一次后重试业务请求。旧刷新令牌不可重复发送；若刷新可能已到服务器但没收到可提交的新令牌对，则保守要求重新授权。NVS 掉电重启后不会重发该令牌。
The pending pairing, device token pair and expiries stay in the device's `cloudzao` NVS namespace, not TF settings backups, history, logs or the BIN. Listing and acknowledgement refresh credentials as needed and retry the operation once on a rejected access token. Do not replay a rotated refresh token; if a request may have reached the server without a new pair being received and saved, reauthorization is required. The old token is not replayed after reboot.

空清单是成功结果：云端返回 `items: []` 时显示「暂无待下载推送」，无需插 TF 卡。连接失败、超时、异常 HTTP 状态和缺失清单不视为空；可重试或根据界面上的 HTTP 状态排查。
An empty `items` array is a successful result and requires no TF card. Connection errors, timeouts, unexpected HTTP statuses and missing list bodies are not treated as empty. Retry or use the displayed HTTP status to investigate.

- **传输**：先点「获取推送列表」，只读取文件名、类型和大小，不下载、写卡或确认。初始不勾选任何文件；可逐项勾选，或用「全选」一次勾选所有页面中的可下载项，再点底栏「下载所选」。列表有文件时，底栏固定显示已选数量。不支持或元数据无效的条目不可勾选。每页 6 项、最多展示前 128 项，未选及超出范围的任务保留在云端。「刷新列表」会清空原勾选，离页或锁屏后也不保留。/ **Transfer:** first select Fetch Push List to view filenames, types and sizes without downloading, writing to the card or acknowledging anything. Nothing is selected by default. Select individual files or use Select All for downloadable items across all pages, then use Download Selected in the fixed footer alongside the selection count. Unsupported or invalid entries cannot be selected. Show six entries per page, up to the first 128; unselected and undisplayed tasks remain on the server. Refresh, leaving the page or locking clears the selection.
- **切换页签**：可点按页签，也可在页签栏或内容区左滑到下一项、右滑到上一项（传输 → 历史记录 → 设置），到两端不循环。与点按一样，接收或检查绑定期间不读取历史。/ **Switch tabs:** tap a tab, or swipe left for the next tab and right for the previous one from the tab strip or content area (Transfer → History → Settings). Swipes stop at the ends. Like tapping, swiping does not open History while receiving or checking binding.
- **下载状态**：只处理所选项。开始下载后显示已写入／总大小、百分比、近期写入速率和整批进度，结果分别统计已保存、待确认、跳过和失败；校验及确认阶段不显示速率，停滞时速率归零。本批列表每页 5 项，显示所选文件的阶段和结果；翻页不打断下载。数值或进度条变化时最多每两秒差分更新一次，本批列表只随任务或阶段变化更新，结束立即显示结果。/ **Download status:** only selected files are processed. The status card shows progress, current and total bytes, recent throughput and batch counts for saved, pending, skipped and failed. No rate is shown during verification or acknowledgement; it drops to zero when stalled. The batch shows stages and results for selected files, five per page. Paging does not interrupt downloads. Live values use differential updates at most once every two seconds; the batch list updates on task or stage changes, and results appear immediately on completion.
- **历史记录**：操作结束后可离线读取当前 TF 卡的最近 32 条文件结果，每页 6 条，最新在前。列表显示文件格式、名称、大小、时间和当时的状态。仅存于 `.readpico/cloudzao/history.v1`；无任务 ID、签名链接或密钥，不重放、不续传。中断或卡写入失败可能使最后一项没有记录，不影响文件校验或云端确认；先前版本的接收不补录。/ **History** reads the current TF card offline after the operation and shows up to 32 recent file results, newest first and six per page. Each row shows the file format, name, size, time and outcome at the time. The file is `.readpico/cloudzao/history.v1`; it has no task ID, signed URL or key and cannot replay or resume transfers. Interrupted operations or card write failures may not leave a record and do not affect verification or acknowledgement. Earlier versions do not backfill history.
  有记录时可点右上角「清除」，再点「确认清除」，离线删除当前 TF 卡的全部 CloudZao 接收记录。不会删除已下载的文件，也不会影响云端推送。若清除失败且卡仍在，列表保留，可重试。/ When history exists, tap **Clear**, then **Confirm clear** to remove all CloudZao receive history on the current TF card offline. This does not delete downloaded files or affect cloud pushes. If clearing fails with the card still present, the list remains and you can retry.
- **设置**：绑定卡显示六位码、等待或授权状态。接收期间可查看，不能发起绑定；等待下一次轮询时可以切换页签，在途请求结束后才能切页签；左上角退出 CloudZao 时取消在途操作并返回文件管理。点「传输」页签只切换内容，不中断接收。/ **Settings:** the binding card shows the six-digit code, waiting or authorization status. View but do not start binding while receiving. Tabs can be switched between polls or after an in-flight request ends; Exiting CloudZao with Back cancels in-flight work and returns to Files. The Transfer tab only switches content without interrupting reception.

设置页底部的「下载 CloudZao App」二维码与云造网页登录页指向同一个[官方下载页](https://yun.cloudzao.com/download)。二维码离线可显示；请用联网的手机扫码。它不包含设备绑定码，也不绑定某个安装包版本。
The **Download CloudZao App** QR code in Settings opens the same [official download page](https://yun.cloudzao.com/download) as the CloudZao web login. The device can show it offline; scan it with an internet-connected phone. It contains no pairing code and does not point to a specific app version.

### 注意事项 / Usage notice

首次打开 CloudZao 时，需点「我已了解」才能进入。确认仅保存在本机，与云造绑定和 TF 卡接收记录相互独立；保存失败会提示重试。之后可在 CloudZao 的「设置 → 注意事项」随时查看。
On first entry, tap “我已了解” (I understand) to proceed. Acknowledgement is saved on the device, independently of pairing and TF card history; if saving fails, the notice offers a retry. Reopen the notice anytime from CloudZao Settings.

截至 **2026 年 11 月 21 日**实测：CloudZao App 仅能上传并推送 TXT、EPUB 原文件；AZW3、MOBI 会被强制转换为 `.ebook`，mogo 不支持接收此格式，请使用 TXT 或 EPUB。CloudZao App 暂时无法推送壁纸，请通过 CloudZao 网页端上传并推送。
Observed on **November 21, 2026**: The CloudZao App can push TXT and EPUB as original files, but converts AZW3 and MOBI to `.ebook`, which mogo cannot receive. Use TXT or EPUB. The CloudZao App cannot currently push wallpapers. Use the CloudZao website to upload and push them.

### 入口标识 / Entry badge

文件管理入口的平滑灰阶徽记改编自 [CloudZao 官方 Logo](https://burner.cloudzao.com/logo.svg)，仅用于识别该功能入口。品牌归原权利人，公开分发前需确认使用权限。
The antialiased grayscale badge adapts the [official CloudZao logo](https://burner.cloudzao.com/logo.svg) to identify this feature. The mark belongs to its rights holder; verify permission before public distribution.

## 文件与确认 / Files and acknowledgement

| 格式 / Format | 目录 / Directory | 单文件上限 / Per-file limit |
| --- | --- | --- |
| EPUB、TXT | `/sdcard/books` | 100 MiB |
| PNG | `/sdcard/pictures` | 20 MiB |
| TTF | `/sdcard/fonts` | 32 MiB |

获取列表只保留易失预览和匹配指纹，不保留任务 ID 或签名地址。确认下载时重新拉取列表，仅用勾选项的任务身份、名称、类型、大小及摘要匹配；重排或换签不影响匹配，移除或变更的项跳过并提示重新获取，新增项不自动下载。签名链接过期时每批最多补拉一次清单，仍只处理原勾选项。签名下载不附带业务凭据。云造若返回 `http://` 的阿里云 OSS 签名链接，设备仍使用同主机 HTTPS（443），只在发送时对 OSS 路径中未转义的合法 UTF-8 字节做百分号编码，保留已有转义和签名查询串，不回退明文；不支持的格式（`.ebook`、A2I1、`.ef` 等）不会确认，独立封面不下载。当前要求清单同时提供有效的大小和 SHA‑256；缺失或不合法不可勾选。仅在校验并保存后逐项确认，HTTP 204 表示成功，超时可重发一次。已有同名文件不覆盖；校验一致时直接确认。已保存计数包含已验证的同名副本，待确认是已保存的子集。失败可在网络、卡或文件冲突恢复后再次手动接收。图片和字体不自动启用。
Preview retains only transient display metadata and fingerprints, not task IDs or signed URLs. Starting a download fetches a new list and matches selected items by task identity, name, type, size and digest. Reordering and refreshed signatures are accepted; removed or changed items are skipped with a refresh hint, and new files are not selected automatically. An expired link triggers at most one extra list fetch per batch, still honoring the original selection. Signed downloads carry no business credentials. For a CloudZao-supplied `http://` Aliyun OSS URL, use HTTPS on the same host (443), with no plaintext fallback. Percent-encode only raw, valid UTF-8 bytes in the OSS path on send, preserving existing escapes and the signed query byte-for-byte. Unsupported formats such as `.ebook`, A2I1 and `.ef` are not acknowledged; separate covers are not fetched. Both valid size and SHA‑256 are required for selection. After verification and saving, acknowledge each file: HTTP 204 is success, and a timeout can be retried once. Same-name files are never overwritten; a verified match can be acknowledged directly. Saved includes verified existing copies; pending is a subset of saved. Retry manually after recovering the network, card or file conflict. Images and fonts are not applied automatically.

OSS 下载专用连接使用 [GlobalSign 官方 R3 根证书](https://secure.globalsign.com/cacert/root-r3.crt)作为信任锚，以兼容下载站提供的交叉签名链；仍校验服务器主机名，不改变业务接口、其他 HTTPS 下载或 mogo OTA 的证书配置。维护时应在下载站换链或 R3 到期前验证新链，不能关闭校验。
The OSS download connection uses the [official GlobalSign R3 root](https://secure.globalsign.com/cacert/root-r3.crt) as its trust anchor for the served cross-signed chain. Hostname checks remain enabled; API calls, other HTTPS downloads and mogo OTA retain their own trust settings. Verify replacement chains before provider rotation or R3 expiry; do not disable verification.

已挂载 TF 卡的后台巡检不会中断接收；介质确认失效后才停止。离页、锁屏、关机、重启和拔卡触发取消；已提交的文件不会撤回，尚未发出的确认停止。TF 卡交接等待句柄归还；超时先保留当前状态。应用侧按 8 秒无进展预算和 5 分钟会话预算检查网络 I/O；SDK 同步 DNS 和卡驱动调用返回前不能强制抢占。接收只清理自身的 `.readpico/cloudzao/incoming.part`，不删除用户文件。
Routine health checks of a mounted card do not interrupt reception; confirmed media loss still stops it. Leaving, locking, powering off, restarting and removing the card cancel the job. Already committed files are retained and unsent acknowledgements stop. Card handoff waits for handles; timeout leaves the current state intact. The app checks an 8-second no-progress budget and a five-minute session budget for network I/O; synchronous SDK DNS and card driver calls cannot be forcibly preempted. The receiver cleans only its own `.readpico/cloudzao/incoming.part`, not user files.

离线回归：`python3 tools/run_reader_baseline.py cloudzao wifi_profiles network_route`。真机分别检查新绑定、重启后授权和已有小程序绑定的重新授权，先测试 TXT、EPUB、TTF，确认获取列表没有写卡／确认，先部分下载验证未选项仍在云端，再全选下载剩余资源并实际打开；独立 App 和小程序各自记录结果，不用其中一项替代另一项。此操作指南不授权刷机或公开分发本机镜像。
Offline regression: `python3 tools/run_reader_baseline.py cloudzao wifi_profiles network_route`. On hardware, verify first pairing, authorization after reboot and reauthorization for an already-bound device; start with TXT, EPUB and TTF pushes. Confirm list fetching does not write or acknowledge; first download a subset and verify the rest remain on the server, then select all remaining items, download and open them. Record standalone app and mini-program results separately. This guide does not authorize flashing or public distribution of local images.
