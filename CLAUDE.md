# esphome-ltc-sync：給 Claude 的維護說明

ESPHome external component（`components/ltc_gen`）：ESP32 產生 SMPTE LTC，用手機熱點 beacon 的 TSF（或 ESP-NOW master）讓多台 node 同步。使用者用繁體中文溝通。

## 開始工作前

1. 先讀 `PROGRESS.md`：目前版本、哪些已在實機驗證、功能測試清單、暫緩項目、設計決策。
2. 不要推翻「設計決策紀錄」裡的決定，除非使用者明確要求；要改就在表格加一列新的決策與原因。

## 進度維護規則

- 每次工作有結果（功能完成、測試通過或失敗、使用者回報實測數據、決策改變），就更新 `PROGRESS.md`：狀態表、測試清單打勾並附結果摘要、最後更新日期。
- 實機狀態只能依使用者提供的數據或 log 標 ✅；沒有就維持 ⬜。模擬或編譯通過不等於實機驗證。
- 發版時同步更新 `CHANGELOG.md`，以及三份根目錄 YAML 的 `project_version`。

## 結構

- `components/ltc_gen/ltc_core.h`：不依賴 ESP-IDF 的核心（LTC 編碼、SyncClock、溫度模型、相位控制、ESP-NOW 封包），可在 PC 上測試
- `components/ltc_gen/ltc_gen.{h,cpp}`：ESPHome / ESP-IDF 膠水（RMT、無線電、混合版電源管理、設定同步）
- `components/ltc_gen/__init__.py`：YAML schema 與驗證
- 根目錄 `ltc-node.yaml`（混合版，預設）、`ltc-node-lowpower.yaml`、`ltc-node-online.yaml`：Athom 風格的產品 YAML
- `examples/`：ESP-NOW node、master、hotspot-keeper
- `test/`：主機測試；`tools/ltc_compare.py`：兩顆 node 立體聲比對工具

## 驗證

```bash
cd test && make libltc && make test        # 單元測試、libltc 比對、24 h 模擬、比對工具
esphome -s ltc_source components config ltc-node.yaml   # 根目錄 YAML 用本機元件
```

改到 C++ 或 YAML 之後，推上 `main` 讓 GitHub CI 完整編譯所有 YAML（本機環境可能連不到 Espressif / PlatformIO 套件庫）。用 `curl https://api.github.com/repos/sinnoken/esphome-ltc-sync/actions/runs?per_page=1` 查結果。

## CI

- CI 只在 `components/`、根目錄 YAML、`examples/`、`test/`、`tools/`、`ci.yml` 有變動時才跑；文件更新不會觸發。
- 把相關修改整理成一次推送，不要為了小改動連續推很多次（每次都會跑 7 個韌體編譯）。
- ESPHome 版本固定在 workflow 的 `ESPHOME_VERSION`；升級要刻意做，並在 PROGRESS.md 記錄。

## Git

- 直接 commit 到 `main` 並推送；commit 訊息用英文。
- 這個環境推不了 tag。發版請使用者在 GitHub 執行 *Actions → Release → Run workflow*（填 tag），workflow 會建立 tag 與 Release 並附上韌體。
