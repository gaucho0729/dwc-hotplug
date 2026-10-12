Linux 5.10のdesignwareのIPを使ったホットプラグ対応のPCIeデバイスドライバーをお願い

Linux 5.10のDesignWare(DWC)ホスト側のグルードライバーとして、リンクアップ/ダウン割り込みとPRSNT#によるホットプラグに対応したものを作ります。まず5.10のDWC APIを実ソースで確認します。

Start installing the arm64 cross compiler in the background while I write the driver.

Core 5.10 already guards child config accesses with a link-up check, so I don't need to wrap map_bus. Writing the driver now.

Got flex built. Now add the driver to the 5.10 tree and build it for arm64 with clang.

Builds clean with `W=1`. Quick checkpatch pass, then the DT binding example and README.

Linux 5.10 の DWC ホスト API に合わせて、ホットプラグ対応の Root Complex グルードライバーを作りました。v5.10 ツリーに組み込み、arm64 向けに clang 18 の `W=1` でビルドしました。警告は出ず、checkpatch も通っています。実機での動作確認はしていません。

**構成の要点**
- **pciehp は使いません。** 多くの DWC SoC は Slot Status 割り込みを配線していないので、ドライバー自身がスロットを管理します。
- **検出方法は3つです。** PRSNT# の GPIO 割り込み、SoC の ```link-up/down``` 割り込み、そのどちらも無い場合のリンク状態のポーリング、の順に使います。
- **挿入時:** 給電 → PERST# を 100 ms 後に解除 → LTSSM 有効 → リンクアップを待つ → 100 ms 待つ → `pci_rescan_bus()`
- **抜去時:** 配下のデバイスを disconnected にする → `pci_stop_and_remove_bus_device()` → 給電 OFF
- **起動時にスロットが空でも挿せます。** PCI fixup で Root Port を `is_hotplug_bridge` にし、後から挿すデバイス用のバス番号とメモリウィンドウを予約します。同時に `native_pcie_hotplug = 0` にして、pciehp と二重に管理しないようにしています。

**SoC に合わせて必ず修正するところ**
- `APP_*` レジスタ定義:LTSSM 有効化、link 割り込みのステータスと許可のレジスタです。値は仮のものです。
- `compatible` 文字列
- PHY を使う SoC では、その初期化処理を追加してください。

**注意点**
- fixup に頼っているため、ビルトイン専用です。モジュールにはできません。
- サプライズ抜去の直後にデバイスドライバーが BAR にアクセスすると、SoC によっては SError になります。DWC の「AXI エラー応答を 0xFFFFFFFF に置き換える」設定を有効にしておくのがおすすめです。

対象の SoC 名(i.MX8M、RK3568 など)を教えてもらえれば、`APP_*` レジスタと PHY の処理をその SoC 用に書き換えます。



# pcie-dw-hotplug — DesignWare PCIe RC ホットプラグ対応ドライバー (Linux 5.10)

## ファイル
| ファイル                | 内容                                                     |
| ---------------------- + ------------------------------------------------------- |
| pcie-dw-hotplug.c      | ドライバー本体（```drivers/pci/controller/dwc/``` に配置）        |
| kconfig-makefile.patch | ```CONFIG_PCIE_DW_HOTPLUG``` を追加する ```Kconfig / Makefile``` 差分 |
| example.dts            | Device Tree 記述例                                       |

## 組み込み
``` sh
sh
cp pcie-dw-hotplug.c linux-5.10/drivers/pci/controller/dwc/
cd linux-5.10 && git apply ../kconfig-makefile.patch
# .config: CONFIG_PCIE_DW_HOTPLUG=y  （ビルトイン専用。PCI fixup はモジュールでは実行されないため）
```

v5.10 タグ・arm64 defconfig・clang 18 (LLVM=1 W=1) でビルド確認済み、checkpatch 警告なし。実機での動作確認はしていません。

## ホットプラグの仕組み

pciehp（Slot Status 割り込み）は多くの DWC SoC で配線されていないため、ドライバー自身でスロットを管理します。

## 検出（あるものを使用、優先順）

1. ```prsnt-gpios（PRSNT#）```の GPIO 割り込み（100 ms デバウンス）
2. SoC の ```link-up / link-down``` 割り込み（```interrupt-names = "hp" + app レジスタ```）
3. どちらも無い場合は PORT_DEBUG1 のリンク状態をポーリング（カーネル引数 ```pcie_dw_hotplug.poll_ms=```、既定 1000 ms）

**挿入** : 給電 → PERST# アサート 100 ms → 解除 → LTSSM 有効 → リンク待ち → 100 ms 待機 → pci_rescan_bus()

**抜去** : 配下デバイスを ```disconnected``` に設定 → ```pci_stop_and_remove_bus_device() → PRSNT#``` が消えていれば LTSSM 停止・PERST# アサート・給電 OFF

**リソース確保** : PCI fixup で Root Port に ```is_hotplug_bridge``` を立てるので、空スロットで起動しても後から挿したデバイス用にバス番号・ウィンドウが予約されます。大きな BAR のデバイスなら ```pci=hpmemsize=64M,hpbussize=4``` などで調整してください。native_pcie_hotplug = 0 にして pciehp との二重管理を防いでいます。

## SoC ごとに必ず直す箇所

* APP_* レジスタ定義（LTSSM enable、link IRQ status/enable、W1C かどうか）— 仮の値です。app を DT に書かなければ LTSSM 制御と link IRQ は使われません（LTSSM がハード/ブートローダーで有効になっている前提）。
* compatible（"vendor,dw-pcie-hp" は仮）
* PHY を使う SoC は ```devm_phy_get() / phy_power_on()``` をプローブに追加
* link_up を SoC の app レジスタで判定したい場合は ```dw_hp_pcie_ops.link_up``` を実装

**注意点**

* サプライズ抜去時の ```SError: 5.10``` のコンフィグアクセスはリンク断を確認してから行いますが、競合は残ります。また抜去直後にデバイスドライバーが BAR に MMIO すると、SoC によっては非ポステッド要求のタイムアウトが SError になります。DWC の「AXI エラー応答を 0xFFFFFFFF に置換」設定（SoC 依存）を有効にすることを推奨します。

* 抜去前に ``echo 1 > /sys/bus/pci/devices/<BDF>/remove``` で論理削除しておくと安全です（その後の再挿入は自動で検出されます）。

* サスペンド/レジューム、```.remove（unbind）```は未実装です（他の 5.10 DWC ドライバーと同様に ```suppress_bind_attrs```）。
