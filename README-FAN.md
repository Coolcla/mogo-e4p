# Fan 韌體整合版

此分支工作目錄依 `chopper1026/mogo-e4p` 建置 Fan 整合韌體，目標為 E4P／ESP32-S31，沿用 app 版號 **1.1.8**。此專案文件列出本機整合功能、建置輸出與目前驗收界線；它不是官方發佈版。

## 建置輸出

兩份 BIN 都從 flash 位址 `0x0` 開始，包含 bootloader、分區表、OTA 初始化資料與 app；它們不是 16 MB Flash 逐位元備份。

| 檔案 | CPU 設定 | 大小 | SHA-256 |
|---|---|---:|---|
| [`outputs/fan.bin`](outputs/fan.bin) | 啟用 DFS，CPU 上限 320 MHz | 4,957,456 bytes | `784e1eba50a38ff4e06562be763a84f772f9c0d1879ad7189cf75168560d6d18` |
| [`outputs/fan-no-dfs.bin`](outputs/fan-no-dfs.bin) | 關閉 DFS，固定 320 MHz | 4,952,208 bytes | `0a92046a2b36392173e7cb57a0dc2b610b4d219ef444311139295a29f0c1b566` |

SHA-256 也列於 [`outputs/sha256.txt`](outputs/sha256.txt)，建置資訊、輸入資產雜湊、分區檢查與 app 餘量記錄在 [`outputs/build-info.txt`](outputs/build-info.txt)。

建置環境為 ESP-IDF 6.1.1（`C:\esp\v6.1.1\esp-idf`）、`C:\Espressif` 工具鏈、RISC-V GCC 15.2.0，使用 `esp32s31 --preview`。兩種設定分別使用獨立 build 與 sdkconfig：

```powershell
$env:IDF_PATH='C:\esp\v6.1.1\esp-idf'
$env:IDF_TOOLS_PATH='C:\Espressif'
. "$env:IDF_PATH\export.ps1"

idf.py --preview -B build/fan-dfs `
  -D 'SDKCONFIG=build/sdkconfig.fan-dfs' `
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s31;sdkconfig.fan-dfs.defaults' build

idf.py --preview -B build/fan-no-dfs `
  -D 'SDKCONFIG=build/sdkconfig.fan-no-dfs' `
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s31;sdkconfig.fan-no-dfs.defaults' build

python tools/merge_fan_images.py
```

## 目前整合項目

- **鍵盤**：英文／注音模式保留候選列下方的數字列；數字模式改用單一數字列並移除「重輸」鍵。鍵帽文字加粗加黑，密碼輸入可切換顯示／隱藏；每輸入五個字元執行一次局部整理，注音與英文可用「中 / En」切換，並保留倒退鍵。
- **閱讀同步設定**：KOReader 帳號、密碼與伺服器欄位採 Wi‑Fi 輸入格樣式，鍵入後立即顯示，密碼可切換顯示／隱藏；KOSync 書籍識別使用 KOReader 相容的 partial MD5；Readest 雲端登入使用一次性代碼與網頁授權。
- **WebDAV**：檔案頁的 USB 傳輸旁提供「WebDAV 傳輸／連結 WebDAV」。首次使用輸入 HTTPS 伺服器網址、使用者名稱和密碼，欄位格式及即時文字刷新與 Wi‑Fi 輸入一致；密碼可切換顯示／隱藏。設定保存在裝置 NVS；之後點選直接連線。可瀏覽目錄、上傳、下載至 TF 卡 `/WebDAV` 資料夾，並對遠端項目重新命名、下載、刪除。匯入圖書也可直接瀏覽 WebDAV；EPUB、TXT、MOBI 可匯入書籍目錄並在下載完成後開啟。伺服器網址可自行設定，不預設綁定單一服務。
- **鍵盤**：底列空格鍵與「中 / En」鍵使用互不重疊的繪製、觸控和局部刷新範圍。
- **USB 傳輸**：主機辨識名稱設為 Fan E4P SD。TF 卡背景探測尚未完成時會保留待啟動狀態並重試；錯誤畫面區分找不到卡、尚未掛載和逾時。USB MSC 沿用 SDMMC 的 DMA 安全中轉緩衝，並使用 8 KiB 高速 USB 工作堆疊。等待電腦連線時提示檢查資料線與 USB 連接埠。
- **Wi-Fi 傳書**：TF 卡目的地的單本書籍上限為 50 MB，裝置端上傳介面與韌體檢查共用此限制；內建快閃空間仍受 1 MB 單檔容量限制。可用空間不足時會先拒絕上傳。
- **檔案操作面板**：本機與 WebDAV 項目採一致的按鍵樣式，按選項數量調整按鍵排列與尺寸；WebDAV 項目使用獨立的確認刪除流程。WebDAV 資料夾可長按管理，點按進入瀏覽。
- **個人資料**：個人名稱預設為「凡繁煩」，可自訂並顯示在頭像旁；預設頭像素材來自 `fan.png`。狀態列簽名維持空白。
- **繁體中文與排版**：系統介面採繁體中文；內建字型子集使用 Noto Sans CJK `wght=500`，完整閱讀字庫由 TF 卡字型檔載入。保留直排標點與字距修正。
- **連線服務**：需要網路的一次性工作依已儲存的網路路由自動取得連線租約，結束、失敗或取消時釋放租約；若 Wi-Fi 由該工作啟動，釋放時會關閉。涵蓋校時、定位、天氣、WebDAV、Readest／KOReader 同步與 CloudZao。長時間 Wi-Fi 傳書依傳輸工作階段維持連線。簡轉繁功能目前暫停，未編入韌體。
- **OTA**：使用者介面不提供 OTA 升級入口；硬體識別、OTA 分區與相容性資料仍保留。

## 驗收狀態

DFS 與固定頻率兩種映像均已建置成功。`ota_0` 與 `ota_1` 各為 5 MiB；本次 app 分區分別剩餘 `0x55cd0` 與 `0x57150` bytes。分區表與 `Fan.zip`、`Fan-usb-full.bin` 的參考配置一致。WebDAV 列目錄暫存資料已移至 PSRAM，避免超過 8 KiB 工作堆疊。簡轉繁目前暫停，未編入韌體。

本次工作沒有刷寫 E4P。USB MSC 的 DMA／堆疊修正已編譯，但電腦枚舉與實際讀寫仍需實機驗收；WebDAV 閃退修正、遠端伺服器相容性、鍵盤刷新、同步帳號流程與氣候顯示也仍待實機驗證。完整建置記錄見 [`outputs/build-info.txt`](outputs/build-info.txt)。
