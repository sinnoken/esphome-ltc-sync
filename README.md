# esphome-ltc-sync

用 ESP32 + ESPHome 做的**無線同步 SMPTE LTC timecode 產生器**。每台相機（和錄音機）各掛一顆，用手機熱點或一顆 ESP32 master 當時間基準，校正後**關掉無線電自己跑**，靠電池就能撐一整天以上。

> ⚠️ 狀態：純省電版、混合版（不含 HA API 的組態）、ESP-NOW node 與 master 都能編譯（ESP-IDF 5.5.6 / ESPHome 2026.10）；含 API 的完整混合版與 online 版只通過設定檢查（建置環境連不到 Espressif / PlatformIO 套件庫），核心演算法有主機端測試與 24 小時閉迴路模擬，但**還沒有在實機上驗證過**。請先照 [docs/verify.md](docs/verify.md) 用兩顆 node 實測，再拿去正式拍攝。

```
 手機熱點（2.4 GHz）           ← 每 102.4 ms 一個 beacon，內含硬體 TSF 時間戳
      │  只在對時視窗被動收聽（不連線、不需密碼）
      ▼
 ┌──────────┐  ┌──────────┐  ┌──────────┐
 │ node 相機1│  │ node 相機2│  │ node 錄音機│   XIAO ESP32C3 + LiPo
 │  RMT→LTC │  │  RMT→LTC │  │  RMT→LTC │   其餘時間無線電關閉，自己跑
 └────┬─────┘  └────┬─────┘  └────┬─────┘
      ▼ 衰減器      ▼            ▼
   相機 mic in    相機 mic in    錄音機空軌        → 後製用 LTC 自動對齊
```

## 特色

- **兩種時間來源**
  - `source: hotspot`（預設）：手機熱點 beacon 的 TSF。不需要 master，node 也不連線。
  - `source: espnow`：一顆 ESP32 跑 `role: master`，每 100 ms 廣播 ESP-NOW 信標（two-step，類似 PTP 的 sync + follow-up）。
- **省電的對時排程**：開機聽 12 秒 → 第 1、5、15 分鐘各再對一次（學 ppm）→ 之後每 `resync_interval` 醒來 3 秒（可設 `0s` 只校正一次）。
- **軟體 TCXO**：C3/S3/C6 內建溫度感測器，每次對時記錄「溫度 ↔ ppm」，學出曲線後自己跑時依溫度修正；曲線存在 flash，隔天開機直接沿用。
- **RMT 硬體輸出**：LTC 雙相波形由 RMT 產生，不吃 CPU 時序；相位控制器帶傳輸延遲補償，速度修正限制在 ±2 %。
- 支援 24 / 25 / 29.97 DF / 30 fps；LTC 編碼與 libltc **逐位元比對一致**，並經 libltc 解碼驗證。
- 附 `tools/ltc_compare.py`：兩顆 node 錄進同一個立體聲檔，直接量出彼此的偏移與飄移，不需示波器。

## 推薦用法

| 優先 | 模式 | 設定 | 一天的誤差（模擬） | 說明 |
|---|---|---|---|---|
| **1（首選）** | **手機熱點整天開著 + 定期對時** | `source: hotspot`、`resync_interval: 10min`（預設） | **約 0.14 ms** | 準度不靠晶振好壞，只要多開一個熱點，成本最低 |
| 2 | 只校正一次 + 溫度補償 | `resync_interval: 0s` | 約 4 ms（模擬，實機可能較差） | 熱點只在開機 15 分鐘內需要；熱點中途斷掉時，模式 1 也會自動退回這個模式 |
| 3 | ESP-NOW master | `source: espnow` | 約 0.14 ms | 沒有手機時使用，master 要一直供電 |
| 4 | 一直連著熱點（online 版） | `source: wifi` | 未模擬 | 方便（HA / OTA），但耗電約 2 倍以上；所有 node 必須連同一台 AP |

模式 1 的注意事項：
- 手機接上行動電源。熱點開一整天，手機本身耗電會明顯增加。
- **熱點不能因為沒人連線而自己關掉或隱藏**：Android 要關掉「自動關閉熱點」；iPhone 在沒有裝置連線時可能會把熱點藏起來，請讓一台裝置一直連著（筆電、平板，或一顆燒 [`examples/hotspot-keeper.yaml`](examples/hotspot-keeper.yaml) 的 ESP32 插在行動電源上）。
- 拍攝中不要重開熱點（TSF 會歸零，timecode 會跳號）。
- 熱點暫時消失時，node 會繼續用溫度補償自己跑，每 60 秒重試一次；只要熱點沒有重開，回來後會無縫接上。

## 模擬結果（假設值，非實測）

24 小時、晶振 +17 ppm、AT-cut 溫度曲線、日夜溫差 10 °C 的模擬（細節與假設見 [docs/accuracy.md](docs/accuracy.md)）：

| 情境 | 24 h 內最大誤差 |
|---|---|
| 每 10 分鐘對時 | **0.14 ms** |
| 只校正一次、無溫度補償、溫差 10 °C | 60 ms |
| 只校正一次、有溫度補償（曲線前一天學過） | **3.9 ms** |
| 只校正一次、恆溫 25 °C | 0.3 ms |

目標「一天 15 ms 以內」用模式 1（定期對時）最穩，因為誤差不會累積；溫度補償的實際效果取決於晶振和溫度感測器，請用 `ltc_compare.py` 實測。

## 設定方式（像 Athom 插座的 YAML 一樣）

repo 根目錄有三份「產品 YAML」，可調的東西都集中在最上面的 `substitutions:`，所有 node 共用同一份（`name_add_mac_suffix: true`，名稱自動加 MAC 後綴）：

| 檔案 | 行為 | 電流（估計） |
|---|---|---|
| [`ltc-node.yaml`](ltc-node.yaml) | **預設，混合版**：開機上線 30 分鐘（HA、網頁、OTA），之後自動進省電模式 | 上線時較高，省電時約 16 mA |
| [`ltc-node-lowpower.yaml`](ltc-node-lowpower.yaml) | 純省電：完全不連 Wi-Fi，只在對時視窗被動收 beacon | 約 16 mA |
| [`ltc-node-online.yaml`](ltc-node-online.yaml) | 一直上線（像插座一樣） | 約 2 倍以上 |

### 混合版的狀態切換

```
開機 ──> 上線（Wi-Fi、HA、網頁、OTA）
          │  開機 30 分鐘後、最後一次改設定 2 分鐘後、或按「Enter Low Power」
          ▼
        省電（Wi-Fi 關閉，每 10 分鐘醒來 3 秒被動收熱點 beacon）
          │  插上電腦 USB、「充電」偵測腳為高、或重新開機
          ▼
        上線（可以 OTA）
```

- Wi-Fi 請設定成**手機熱點**（2.4 GHz），所有 node 連同一個。上線時就在熱點的頻道上收 beacon，省電時收的也是同一個熱點，兩種模式用同一個時間基準，LTC 輸出全程不中斷。
- 第一次設定 Wi-Fi：像 Athom 插座一樣，用 ESPHome 的 improv（USB）或 node 自己開的設定熱點 `ltc-node-xxxxxx`（captive portal）。
- OTA 進行中、插著電腦 USB 時不會進省電模式。要偵測「接上充電器」（沒有資料線也算），可以把 XIAO 的 5V 腳經 100 kΩ/100 kΩ 分壓接到 GPIO3，YAML 裡已經有註解掉的範例。
- 需要 `api: reboot_timeout: 0s` 和 `wifi: reboot_timeout: 0s`（範本已設好；少了會在設定檢查時擋下來），否則 Wi-Fi 關掉後會被自動重開機。

### 設定同步：改一台，全部跟著變

在 **任何一台上線中的 node**（HA 或它的網頁）改下列設定，其他同一個 `sync_group` 的 node 都會跟著換，包括省電中的 node（最慢一個對時間隔內）和純省電版：

| 同步的設定 | HA 實體 |
|---|---|
| 影格率 | Frame Rate |
| 從 start_timecode 重新開始計時 | Jam Timecode（按鈕） |
| 對時間隔 | Resync Interval |
| 開機上線時間 | Online Time After Power-on |

- 上線中的 node 每秒用 ESP-NOW 在熱點頻道廣播一次設定；省電中的 node 在對時視窗被動收下，**仍然只收不發**。
- 每次修改版本號加 1，最新的版本勝出；設定存在 flash，重開機後還在。YAML 裡的值只是第一次開機的預設值；要回到 YAML 的值，按「Reset Synced Settings」後重開機。
- 不要在拍攝途中改 fps 或 jam：各台套用的時間點最多差一個對時間隔。
- 至少要有一台在上線狀態才能發出設定（剛開機的 30 分鐘內，或插上 USB 的那台）。

### 燒錄

1. 改 `ltc-node.yaml` 最上面的 `substitutions`（元件會從 `github://sinnoken/esphome-ltc-sync` 下載）。
2. 第一次用 USB：ESPHome Dashboard *Install → Manual download → Factory format*，到 [web.esphome.io](https://web.esphome.io) 每顆燒同一個 `.bin`；或 `esphome run ltc-node.yaml`。
3. 之後混合版可以在上線期間 OTA；純省電版只能用 USB。

### 拍攝現場

1. 手機熱點設成 **2.4 GHz**（iPhone：「最大化相容性」；Android：選 2.4 GHz 頻段、關閉「自動關閉熱點」）。
2. 接線：見 [docs/hardware.md](docs/hardware.md)（GPIO → 電阻分壓 + 隔直電容 → 相機 mic 輸入）。
3. 開熱點（iPhone 保持在「個人熱點」設定頁，或讓一台裝置連著），node 開機，約 15 秒後鎖定（LED 亮）。之後 15 分鐘內完成 ppm 校正。
4. 相機關閉 AGC，把 LTC 錄在一個聲道，電平約 -20 dBFS。
5. 後製：DaVinci Resolve 選片段 → *Clip Attributes / Update Timecode from Audio Track*，或用 Tentacle Sync Studio 等工具依 LTC 對齊。

沒有手機時改用 ESP-NOW：一顆 ESP32 燒 [`examples/ltc-master.yaml`](examples/ltc-master.yaml)，node 用 [`examples/ltc-node-espnow.yaml`](examples/ltc-node-espnow.yaml)。要接 Home Assistant、用 NTP 時刻當 timecode，用 [`examples/ltc-master-wifi.yaml`](examples/ltc-master-wifi.yaml)。

## 在你自己的 YAML 裡使用

```yaml
external_components:
  - source: github://sinnoken/esphome-ltc-sync
    components: [ltc_gen]

esp32:
  board: seeed_xiao_esp32c3
  cpu_frequency: 80MHZ
  framework:
    type: esp-idf

ltc_gen:
  id: ltc
  pin: GPIO5
  hotspot_ssid: "My iPhone"
  fps: "25"
```

## 設定參考

| 選項 | 預設 | 說明 |
|---|---|---|
| `pin` | 必填 | LTC 輸出腳位 |
| `role` | `node` | `node` 或 `master` |
| `source` | `hotspot` | node 的時間來源：`hotspot` / `espnow` / `wifi`（需要 `wifi:` 元件，讀已連線 AP 的 TSF） |
| `hotspot_ssid` | — | `source: hotspot` 時必填 |
| `channel` | 自動 | hotspot：留空則掃描；espnow：須與 master 相同（預設 1） |
| `fps` | `30` | `24`、`25`、`29.97df`、`30`（espnow 模式由 master 決定） |
| `initial_window` | `12s` | 開機第一次對時的收聽長度（≥ 9 s，用來估 ppm） |
| `window` | `3s` | 之後每次對時的收聽長度 |
| `resync_interval` | `10min`（`wifi` 為 `1min`） | 定期對時間隔；`0s` = 只在開機後 0/1/5/15 分鐘校正，之後完全自己跑 |
| `temperature_compensation` | `true` | 用內建溫度感測器做 ppm 補償（原版 ESP32 無此硬體，自動略過） |
| `persist_temperature_model` | `true` | 把學到的溫度曲線存進 flash |
| `output_offset_us` | `0` | 固定延遲修正（µs），用來抵銷不同板子/接法的常數差 |
| `start_timecode` | `00:00:00:00` | hotspot：TSF 0 時的 timecode；master：開機時的 timecode |
| `time_id` | — | master 專用：用 `time:` 元件的當地時刻當 timecode |
| `online_timeout` | `30min` | 混合版：開機後上線多久（`0s` = 直接進省電） |
| `online_after_change` | `2min` | 混合版：最後一次改設定後多久進省電 |
| `usb_keeps_online` | `true` | 混合版：插著電腦 USB 時保持上線 |
| `settings_sync` | `true` | 是否參與 ESP-NOW 設定同步（hotspot / wifi node） |
| `sync_group` | `ltc` | 同一組的 node 才互相同步設定 |
| `pps_pin` | — | 每秒輸出一個 1 幀寬的脈衝，給示波器驗證用 |
| `invert` | `false` | 反相輸出 |
| `timecode` / `sync_state` | — | text sensor |
| `locked` | — | binary sensor（可驅動 LED） |
| `ppm` / `sync_error` / `phase_error` / `temperature` | — | 診斷用 sensor |

Lambda API：`set_fps("25")`、`jam()`、`sync_now()`、`set_resync_minutes(x)`、`set_online_minutes(x)`、`enter_low_power()`、`go_online()`、`set_charging(bool)`、`set_ota_active(bool)`、`reset_settings()`、`timecode()`、`is_locked()`、`is_online()`（皆為 `id(ltc).…`）。

限制：`espnow` 的 node 不能同時使用 `wifi:`；`hotspot` 加上 `wifi:` 就是混合版；不能與 `espnow:`、`internal_temperature` 一起用；需要 ESP-IDF framework。

## 文件

- [docs/hardware.md](docs/hardware.md)：板子、電池、接線與電平
- [docs/accuracy.md](docs/accuracy.md)：原理、誤差來源、模擬假設與結果
- [docs/verify.md](docs/verify.md)：上線前的實測步驟（`ltc_compare.py`）
- [PROGRESS.md](PROGRESS.md)：目前進度、功能測試清單、待辦與設計決策

## 開發

核心 (`components/ltc_gen/ltc_core.h`) 不依賴 ESP-IDF，可在 PC 上測：

```bash
cd test
make libltc     # 下載 x42/libltc 作為獨立參考實作
make test       # 單元測試、libltc 比對/解碼、24 h 模擬、比對工具測試
```

## 授權

MIT。測試時使用的 [libltc](https://github.com/x42/libltc)（LGPL-3.0）只在主機端測試中編譯，不包含在韌體內。
