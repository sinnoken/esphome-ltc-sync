# 上線前實測

## 需要的東西

- 兩顆燒好 node 韌體的板子（同樣的設定、不同的 `name`）
- 一台能錄立體聲的錄音機，或電腦的 line-in（48 kHz 以上）
- 兩條衰減線（line level 版，見 [hardware.md](hardware.md)）

## 步驟

1. node A 接錄音機左聲道，node B 接右聲道。兩個聲道用**同一台**錄音機錄，錄音機自己的時脈誤差會互相抵銷。
2. 開手機熱點，兩顆 node 開機，確認 LED 都亮（locked）。
3. 開始錄音，一直錄到你想驗證的時間長度：
   - 想驗證**定期對時**：錄 1–2 小時，看整段的最大誤差。
   - 想驗證**只校正一次**：設 `resync_interval: 0s`，第 15 分鐘後**關掉熱點**，錄一整個工作天。最好帶到實際拍攝環境（溫度會變）。
4. 分析：

   ```bash
   pip install numpy matplotlib
   python3 tools/ltc_compare.py recording.wav --plot offset.png --csv offset.csv
   ```

   輸出範例：

   ```
   recording.wav: 480.0 min at 48000 Hz
     channel A: 719998 frames decoded, channel B: 719999 frames decoded
     matched frames: 719998  (100.0 % of A)
     (positive offset = B starts its frames later than A)
     offset B-A: median 12.4 us, min -310.2 us, max 402.0 us
     peak-to-peak 712.2 us, std 140.3 us
     trend +0.0004 us/s = +0.03 ms/day (B vs A: -0.0004 ppm fast)
     (1 frame = 40000 us)
   ```

5. 判斷：
   - `matched frames` 接近 100 %：兩顆的幀號一致。
   - `offset` 的範圍就是兩顆之間的同步誤差。兩顆對同一個參考，所以「單顆相對參考的誤差」大約是這個值的一半到 0.7 倍。
   - `median` 不為 0 而且一直固定：是常數延遲，可以用 `output_offset_us` 修掉（例如 B 設 `-12`）。
   - `trend` 換算成 ms/day，就是只校正一次時的實際飄移速度。

## 單機檢查（示波器）

設定 `pps_pin`，每秒第 0 幀開始時輸出一個寬 1 幀的高電位。兩顆接示波器兩個通道、用 CH1 觸發，就能直接看到兩顆之間的時間差。

## 序列埠 log

node 沒有 Wi-Fi，診斷資訊在 USB 序列埠：`esphome logs examples/ltc-node.yaml`。
`timecode`、`ppm`、`sync_error`（每次對時量到的預測誤差）、`temperature` 都會每秒印出。`sync_error` 長期在 ±200 µs 以內代表一切正常。
