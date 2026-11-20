# CloudZao 传输联调 / CloudZao transfer testing

默认构建不提供此入口。此功能用于单机联调，不是正式 OTA 来源，不替代 WiFi／热点浏览器传书或 USB。不轮询，不走 4G，不替用户开关无线。
The default build omits this entry. It is a single-device test, not an OTA source or a replacement for WiFi/hotspot browser transfer or USB. It does not poll, use cellular or change radio settings.

## 启用与使用 / Enable and use

先按[本机环境规则](../../ONBOARDING.md#本机环境硬约束--local-environment-hard-rules)验证 ESP-IDF 6.1、Python 3.13 和 S31。使用独立配置和被忽略的构建目录，不将联调选项写入产品 defaults。
Validate ESP-IDF 6.1, Python 3.13 and S31 using onboarding first. Keep the test option in a separate config and ignored build directory, not in product defaults.

```sh
mkdir -p build/cloudzao-check
printf 'CONFIG_MOGO_CLOUDZAO_TRANSFER=y\n' > build/cloudzao-check/cloudzao.defaults
IDF_TARGET=esp32s31 idf.py --preview -B build/cloudzao-check/firmware \
  -D SDKCONFIG="$PWD/build/cloudzao-check/sdkconfig.cloudzao" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;$PWD/build/cloudzao-check/cloudzao.defaults" build
```

从[原厂开发者文档](https://burner.cloudzao.com/zh/ota-docs)进入个人中心取得单机测试密钥。设备先连接 WiFi，并插入已挂载的 TF 卡。在「文件管理」的「传输文件」区域直接点第四项「CloudZao」，在「设置」页签点「设置联调密钥」。手输密钥始终掩码，离开接收页、锁屏或重启后需重输；不保存到 TF 卡、NVS 或备份。已绑定时可直接接收；未绑定时点「检查绑定 / 获取绑定码」，在云造端完成绑定。若云造未返回验证码，设备只读查询一次资源列表；只有查询成功（即使列表为空）才显示「已绑定」，否则提示无法确认或请求失败。这个检查不下载、写卡或移除任务。
Get a test key from the developer account linked by the vendor docs. Connect to WiFi and insert a mounted TF card. Under Transfer Files in File Manager, tap the fourth entry, CloudZao, then set the test key under Settings. A manually entered key is masked and must be re-entered after leaving the receiver, locking or restarting; it is not saved to TF, NVS or backups. Already-bound devices can receive directly; others use Check Binding / Get Code and complete binding on the CloudZao side. If no code is returned, the receiver reads the resource list once to verify binding. Only a successful list response, including an empty list, confirms binding. This check does not download, write files or remove tasks.

- **传输**：先点「获取推送列表」，只读取文件名、类型和大小，不下载、写卡或确认。初始不勾选任何文件；可逐项勾选，或用「全选」一次勾选所有页面中的可下载项，再点底栏「下载所选」。列表有文件时，底栏固定显示已选数量。不支持或元数据无效的条目不可勾选。每页 6 项、最多展示前 128 项，未选及超出范围的任务保留在云端。「刷新列表」会清空原勾选，离页或锁屏后也不保留。/ **Transfer:** first select Fetch Push List to view filenames, types and sizes without downloading, writing to the card or acknowledging anything. Nothing is selected by default. Select individual files or use Select All for downloadable items across all pages, then use Download Selected in the fixed footer alongside the selection count. Unsupported or invalid entries cannot be selected. Show six entries per page, up to the first 128; unselected and undisplayed tasks remain on the server. Refresh, leaving the page or locking clears the selection.
- **切换页签**：可点按页签，也可在页签栏或内容区左滑到下一项、右滑到上一项（传输 → 历史记录 → 设置），到两端不循环。与点按一样，接收或检查绑定期间不读取历史。/ **Switch tabs:** tap a tab, or swipe left for the next tab and right for the previous one from the tab strip or content area (Transfer → History → Settings). Swipes stop at the ends. Like tapping, swiping does not open History while receiving or checking binding.
- **下载状态**：只处理所选项。开始下载后显示已写入／总大小、百分比、近期写入速率和整批进度，结果分别统计已保存、待确认、跳过和失败；校验及确认阶段不显示速率，停滞时速率归零。本批列表每页 5 项，显示所选文件的阶段和结果；翻页不打断下载。数值或进度条变化时最多每两秒差分更新一次，本批列表只随任务或阶段变化更新，结束立即显示结果。/ **Download status:** only selected files are processed. The status card shows progress, current and total bytes, recent throughput and batch counts for saved, pending, skipped and failed. No rate is shown during verification or acknowledgement; it drops to zero when stalled. The batch shows stages and results for selected files, five per page. Paging does not interrupt downloads. Live values use differential updates at most once every two seconds; the batch list updates on task or stage changes, and results appear immediately on completion.
- **历史记录**：操作结束后可离线读取当前 TF 卡的最近 32 条文件结果，每页 6 条，最新在前。列表显示文件格式、名称、大小、时间和当时的状态。仅存于 `.readpico/cloudzao/history.v1`；无任务 ID、签名链接或密钥，不重放、不续传。中断或卡写入失败可能使最后一项没有记录，不影响文件校验或云端确认；先前版本的接收不补录。/ **History** reads the current TF card offline after the operation and shows up to 32 recent file results, newest first and six per page. Each row shows the file format, name, size, time and outcome at the time. The file is `.readpico/cloudzao/history.v1`; it has no task ID, signed URL or key and cannot replay or resume transfers. Interrupted operations or card write failures may not leave a record and do not affect verification or acknowledgement. Earlier versions do not backfill history.
  有记录时可点右上角「清除」，再点「确认清除」，离线删除当前 TF 卡的全部 CloudZao 接收记录。不会删除已下载的文件，也不会影响云端推送。若清除失败且卡仍在，列表保留，可重试。/ When history exists, tap **Clear**, then **Confirm clear** to remove all CloudZao receive history on the current TF card offline. This does not delete downloaded files or affect cloud pushes. If clearing fails with the card still present, the list remains and you can retry.
- **设置**：设备绑定卡提供「检查绑定 / 获取绑定码」，并在卡内显示绑定状态或六位绑定码。接收期间仍可查看，但不能发起绑定。只有直连联调构建提供会话密钥编辑；中转构建不展示服务配置。三个页签的左上角返回均直接回到文件管理并取消当前操作；点「传输」页签则只切换内容，不中断接收。/ **Settings** has a device binding card with a check/code action and the resulting binding state or six-digit code. You can view it while receiving, but cannot start binding. Only direct test builds expose session key editing; the relay build does not show service configuration. The top-left Back on any tab returns to Files and cancels the current operation; the Transfer tab only switches content without interrupting reception.

### 入口标识 / Entry badge

文件管理入口的平滑灰阶徽记改编自 [CloudZao 官方 Logo](https://burner.cloudzao.com/logo.svg)，仅用于识别该联调入口；默认构建不包含该徽记。品牌归原权利人，公开分发前需确认使用权限。
The antialiased grayscale badge adapts the [official CloudZao logo](https://burner.cloudzao.com/logo.svg) to identify this test entry. It is omitted from the default build. The mark belongs to its rights holder; verify permission before public distribution.

### 仅本机：通过 mogo-platform 中转 / Local only: use the mogo-platform relay

使用 `build/cloudzao-relay-local/firmware` 构建，启用 `MOGO_CLOUDZAO_LOCAL_RELAY=ON` 并禁用编译缓存。`build/cloudzao-relay-local/private/` 中提供 `cloudzao_local_relay.h`（`CZ_RELAY_ADDRESS`、`CZ_RELAY_PORT`、`CZ_RELAY_TLS_NAME`、`CZ_RELAY_AUTHORITY` 和 `CZ_RELAY_DEVICE_TOKEN`）及 `cloudzao_relay_ca.pem`。HTTPS 证书名称必须与 `CZ_RELAY_TLS_NAME` 一致。设备与后端须能通过同一局域网通信；设备的 CloudZao「设置」只显示设备绑定，不需要录入开发者密钥。
Build in `build/cloudzao-relay-local/firmware` with `MOGO_CLOUDZAO_LOCAL_RELAY=ON` and with compiler caching off. Supply `cloudzao_local_relay.h` (with the five macros listed above) and `cloudzao_relay_ca.pem` under `build/cloudzao-relay-local/private/`. The HTTPS certificate name must match `CZ_RELAY_TLS_NAME`. The device must reach the backend on the LAN. CloudZao Settings only shows device binding; no developer key needs to be entered on the device.

后端按设备凭据摘要鉴权，从服务端配置读取开发者密钥和设备 WiFi MAC。绑定、拉列表和确认走中转，签名文件仍由设备直接下载；中转 CA 与 Bearer 不用于下载。镜像中的单机凭据可提取，联调结束后关闭中转并撤销凭据，不分发镜像。此选项不用于 CI 或正式版，不代表已支持公开中转。
The backend authenticates against the device token digest and reads the developer key and WiFi MAC from server configuration. Binding, listing and acknowledgement use the relay; signed files are downloaded directly. The relay CA and Bearer are not used for downloads. The embedded device token is extractable. Shut the relay down and revoke the token after testing; do not distribute the image. This option is not for CI, releases or public relay use.

## 文件与确认 / Files and acknowledgement

| 格式 / Format | 目录 / Directory | 单文件上限 / Per-file limit |
| --- | --- | --- |
| EPUB、TXT | `/sdcard/books` | 100 MiB |
| PNG | `/sdcard/pictures` | 20 MiB |
| TTF | `/sdcard/fonts` | 32 MiB |

获取列表只保留易失预览和匹配指纹，不保留任务 ID 或签名地址。确认下载时重新拉取列表，仅用勾选项的任务身份、名称、类型、大小及摘要匹配；重排或换签不影响匹配，移除或变更的项跳过并提示重新获取，新增项不自动下载。签名下载不附带业务凭据。云造若返回 `http://` 的阿里云 OSS 签名链接，设备仍使用同主机 HTTPS（443），不改路径和签名参数，不回退明文；不支持的格式（`.ebook`、A2I1、`.ef` 等）不会确认，独立封面不下载。校验大小和 SHA‑256 后才发布文件并确认。已有同名文件不覆盖；校验一致时直接确认。已保存计数包含已验证的同名副本，待确认是已保存的子集。失败可在网络、卡或文件冲突恢复后再次手动接收。图片和字体不自动启用。
Preview retains only transient display metadata and fingerprints, not task IDs or signed URLs. Starting a download fetches a new list and matches selected items by task identity, name, type, size and digest. Reordering and refreshed signatures are accepted; removed or changed items are skipped with a refresh hint, and new files are not selected automatically. Signed downloads carry no business credentials. For a CloudZao-supplied `http://` Aliyun OSS URL, use HTTPS on the same host (443), preserving the path and signature parameters with no plaintext fallback. Unsupported formats such as `.ebook`, A2I1 and `.ef` are not acknowledged; separate covers are not fetched. Only files verified by size and SHA‑256 are published then acknowledged. Same-name files are never overwritten; a verified match can be acknowledged directly. Saved includes verified existing copies; pending is a subset of saved. Retry manually after recovering the network, card or file conflict. Images and fonts are not applied automatically.

OSS 下载专用连接使用 [GlobalSign 官方 R3 根证书](https://secure.globalsign.com/cacert/root-r3.crt)作为信任锚，以兼容下载站提供的交叉签名链；仍校验服务器主机名，不改变业务接口、其他 HTTPS 下载或 mogo OTA 的证书配置。维护时应在下载站换链或 R3 到期前验证新链，不能关闭校验。
The OSS download connection uses the [official GlobalSign R3 root](https://secure.globalsign.com/cacert/root-r3.crt) as its trust anchor for the served cross-signed chain. Hostname checks remain enabled; API calls, other HTTPS downloads and mogo OTA retain their own trust settings. Verify replacement chains before provider rotation or R3 expiry; do not disable verification.

已挂载 TF 卡的后台巡检不会中断接收；介质确认失效后才停止。离页、锁屏、关机、重启和拔卡触发取消；已提交的文件不会撤回，尚未发出的确认停止。TF 卡交接等待句柄归还；超时先保留当前状态。应用侧按 8 秒无进展预算和 5 分钟会话预算检查网络 I/O；SDK 同步 DNS 和卡驱动调用返回前不能强制抢占。接收只清理自身的 `.readpico/cloudzao/incoming.part`，不删除用户文件。
Routine health checks of a mounted card do not interrupt reception; confirmed media loss still stops it. Leaving, locking, powering off, restarting and removing the card cancel the job. Already committed files are retained and unsent acknowledgements stop. Card handoff waits for handles; timeout leaves the current state intact. The app checks an 8-second no-progress budget and a five-minute session budget for network I/O; synchronous SDK DNS and card driver calls cannot be forcibly preempted. The receiver cleans only its own `.readpico/cloudzao/incoming.part`, not user files.

离线回归：`python3 tools/run_reader_baseline.py cloudzao network_route`。真机分别检查既有绑定和新绑定，推送四类资源，确认获取列表没有写卡／确认，先部分下载验证未选项仍在云端，再全选下载剩余资源并实际打开；独立 App 和小程序各自记录结果，不用其中一项替代另一项。此操作指南不授权刷机或公开分发联调包。
Offline regression: `python3 tools/run_reader_baseline.py cloudzao network_route`. On hardware, verify existing and new bindings, then push all four formats. Confirm list fetching does not write or acknowledge; first download a subset and verify the rest remain on the server, then select all remaining items, download and open them. Record standalone app and mini-program results separately. This guide does not authorize flashing or public distribution of test builds.
