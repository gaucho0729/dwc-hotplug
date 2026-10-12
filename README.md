# pcie-dw-hotplug — DesignWare PCIe RC ホットプラグ対応ドライバー (Linux 5.10)

## ファイル
| ファイル | 内容 |
|---|---|
| `pcie-dw-hotplug.c` | ドライバー本体（`drivers/pci/controller/dwc/` に配置） |
| `kconfig-makefile.patch` | `CONFIG_PCIE_DW_HOTPLUG` を追加する Kconfig / Makefile 差分 |
| `example.dts` | Device Tree 記述例 |

## 組み込み
```sh
cp pcie-dw-hotplug.c linux-5.10/drivers/pci/controller/dwc/
cd linux-5.10 && git apply ../kconfig-makefile.patch
# .config: CONFIG_PCIE_DW_HOTPLUG=y  （ビルトイン専用。PCI fixup はモジュールでは実行されないため）
```
v5.10 タグ・arm64 defconfig・clang 18 (`LLVM=1 W=1`) でビルド確認済み、checkpatch 警告なし。実機での動作確認はしていません。

## ホットプラグの仕組み
pciehp（Slot Status 割り込み）は多くの DWC SoC で配線されていないため、ドライバー自身でスロットを管理します。

**検出**（あるものを使用、優先順）
1. `prsnt-gpios`（PRSNT#）の GPIO 割り込み（100 ms デバウンス）
2. SoC の link-up / link-down 割り込み（`interrupt-names = "hp"` + `app` レジスタ）
3. どちらも無い場合は `PORT_DEBUG1` のリンク状態をポーリング（カーネル引数 `pcie_dw_hotplug.poll_ms=`、既定 1000 ms）

**挿入**: 給電 → PERST# アサート 100 ms → 解除 → LTSSM 有効 → リンク待ち → 100 ms 待機 → `pci_rescan_bus()`

**抜去**: 配下デバイスを disconnected に設定 → `pci_stop_and_remove_bus_device()` → PRSNT# が消えていれば LTSSM 停止・PERST# アサート・給電 OFF

**リソース確保**: PCI fixup で Root Port に `is_hotplug_bridge` を立てるので、空スロットで起動しても後から挿したデバイス用にバス番号・ウィンドウが予約されます。大きな BAR のデバイスなら `pci=hpmemsize=64M,hpbussize=4` などで調整してください。`native_pcie_hotplug = 0` にして pciehp との二重管理を防いでいます。

## SoC ごとに必ず直す箇所
- **`APP_*` レジスタ定義**（LTSSM enable、link IRQ status/enable、W1C かどうか）— 仮の値です。`app` を DT に書かなければ LTSSM 制御と link IRQ は使われません（LTSSM がハード/ブートローダーで有効になっている前提）。
- **`compatible`**（`"vendor,dw-pcie-hp"` は仮）
- PHY を使う SoC は `devm_phy_get()` / `phy_power_on()` をプローブに追加
- `link_up` を SoC の app レジスタで判定したい場合は `dw_hp_pcie_ops.link_up` を実装

## 注意点
- **サプライズ抜去時の SError**: 5.10 のコンフィグアクセスはリンク断を確認してから行いますが、競合は残ります。また抜去直後にデバイスドライバーが BAR に MMIO すると、SoC によっては非ポステッド要求のタイムアウトが SError になります。DWC の「AXI エラー応答を 0xFFFFFFFF に置換」設定（SoC 依存）を有効にすることを推奨します。
- 抜去前に `echo 1 > /sys/bus/pci/devices/<BDF>/remove` で論理削除しておくと安全です（その後の再挿入は自動で検出されます）。
- サスペンド/レジューム、`.remove`（unbind）は未実装です（他の 5.10 DWC ドライバーと同様に `suppress_bind_attrs`）。
