# レシピ転送フォーマット仕様 v1

esp32p4-cam（wifi-app／usb-app共通）．2026年11月17日制定．同日改訂：値の書き方をボードに合わせた，レシピの各ステップに解決済みの `values` を追加，ラン計画の更新を `POST /run/update` に分けた．

プリセットとレシピを，メンバー間でファイルとして受け渡すための形式を定義する．Web UIのエクスポートはこの形式でJSONを出力し，受け取った側はこの文書だけで構造を理解できる．撮影データセット側が保持する条件マニフェストの形式もあわせて定義する．

## 1．バンドルファイル

プリセットとレシピの受け渡しは，1つのJSONファイル（バンドル）で行う．

- ファイル名：`recipe-bundle_YYYYMMDD-HHMMSS.json`
- 文字コード：UTF-8
- 人が読む前提のファイルなので，インデント2スペースで整形して改行を入れる（機械可読性は変わらない）
- 構造：

```json
{
  "format": "esp32p4-cam/recipe-bundle",
  "version": 1,
  "exported_at": "2026-11-17T08:30:00+09:00",
  "presets": [ ],
  "recipes": [ ]
}
```

- `format` と `version` は必須．受け取り側は `format` が一致し，`version` が対応範囲内であることを確認する
- `presets` と `recipes` は，どちらか一方だけでもよい（渡すものだけを含む）

## 2．プリセット

名前付きの撮影条件の集合．キーと値の書き方は，ボードの `/ctrl`（`usb-app/API.md`）と同一とする．

```json
{
  "name": "標準_屋内",
  "values": {
    "exposure_mode": "manual",
    "exposure_us": 10000,
    "gain_mode": "manual",
    "gain_index": 10,
    "focus_mode": "manual",
    "focus_pos": 240,
    "wb_mode": "manual",
    "wb_red": 1600,
    "wb_blue": 1400,
    "sp": "B",
    "shot_q": 95,
    "shot_light": true
  }
}
```

- 項目は12項目．カメラ条件9項目（exposure_mode，exposure_us，gain_mode，gain_index，focus_mode，focus_pos，wb_mode，wb_red，wb_blue）に，sp，shot_q（撮影画質），shot_light（撮影時の照明一括ON/OFF）を加えたもの
- **向き（hflip，vflip）は含めない**．2026年11月16日の決定による．適用で再起動が起きないようにするためである
- 値の書き方：

| 項目 | 値 |
| --- | --- |
| `exposure_mode`，`gain_mode`，`focus_mode`，`wb_mode` | `"auto"` または `"manual"` |
| `exposure_us` | シャッター時間（µs，整数） |
| `gain_index` | ゲインの番号（整数．倍率はボードの `GET /ctrl/caps` の表で引く） |
| `focus_pos` | レンズ位置（整数．大きいほど近く） |
| `wb_red`，`wb_blue` | WBのゲイン（整数．1000 = ×1.0） |
| `sp` | シャッター優先の方式．`"A"` または `"B"`（通常は `"B"`） |
| `shot_q` | 撮影の JPEG 画質（1〜100） |
| `shot_light` | 撮影時に照明を点けるか（`true`／`false`） |

- 各項目の値域はボードの `GET /ctrl/caps` が返す範囲に従う．この文書では値域を固定しない．範囲外の値はボードが丸める
- `manual` のときだけ値が効く．たとえば `exposure_mode` が `"manual"` で `gain_mode` が `"auto"` だと，ボードはゲインを自動で合わせて明るさを保つ（シャッター優先）
- 一部の項目だけを持つプリセットも許す．その場合，無い項目は適用時に変更しない

## 3．レシピ

複数条件の連続撮影の手順．土台の条件に，ステップごとの上書きを重ねて実行する．**バンドルでは，各ステップに解決済みの条件（`values`）も書くので，受け取り側は継承を解決しなくてよい．**

```json
{
  "name": "露出5段",
  "base_preset": "標準_屋内",
  "base_values": {
    "exposure_mode": "manual", "exposure_us": 10000,
    "gain_mode": "manual", "gain_index": 10,
    "focus_mode": "manual", "focus_pos": 240,
    "wb_mode": "manual", "wb_red": 1600, "wb_blue": 1400,
    "sp": "B", "shot_q": 95, "shot_light": true
  },
  "steps": [
    {
      "enabled": true,
      "overrides": { "exposure_mode": "manual", "gain_mode": "manual", "exposure_us": 2500 },
      "values": {
        "exposure_mode": "manual", "exposure_us": 2500,
        "gain_mode": "manual", "gain_index": 10,
        "focus_mode": "manual", "focus_pos": 240,
        "wb_mode": "manual", "wb_red": 1600, "wb_blue": 1400,
        "sp": "B", "shot_q": 95, "shot_light": true
      },
      "light_out": 255, "q": 95, "settle_ms": 800
    },
    {
      "enabled": true,
      "overrides": { "exposure_mode": "manual", "gain_mode": "manual", "exposure_us": 5000 },
      "values": { "exposure_mode": "manual", "exposure_us": 5000, "…": "（12項目．以下略）" },
      "light_out": 255, "q": 95, "settle_ms": 800
    }
  ]
}
```

（例の2ステップ目以降は省略表記．実際のファイルでは，すべてのステップが12項目の `values` を持つ．）

### フィールドの意味

- `base_preset`：土台にしたプリセット名（由来を示す情報）．`null` の場合は，書き出し時点のボードの現在設定を土台にしたことを示す
- `base_values`：土台の12項目．書き出しのときに，その時点の土台の内容を埋め込む．`base_preset` があればそのプリセットの内容（部分項目なら書き出し時点の現在値で補完），`null` なら書き出し時点の現在値．**転送ファイルでは必須で，12項目すべてを含む**
- `steps`：実行順の配列．各ステップは次を持つ
  - `enabled`：falseのステップは実行しない（定義は残る）
  - `overrides`：土台に対して上書きする項目だけを書く（作り手の意図．何を振っているレシピかが分かる）．上書きできるのはカメラ条件の9項目だけで，`sp`・`shot_q`・`shot_light` は上書きしない（画質はステップの `q` で指定する）
  - `values`：`base_values` に `overrides` を重ねた，**そのステップの解決済みの12項目**．受け取り側はこれだけ読めばよい．転送ファイルでは必須
  - `light_out`：照明出力マスク（0〜255）．ビットがOUT0〜OUT7に対応する．`null` の場合は照明を変更しない
  - `q`：JPEG画質．省略時は95
  - `settle_ms`：条件適用後に撮影するまでの待機時間（ミリ秒）．省略時は800
- ボード内部の保存では，`base_values` と `values` を持たない形（土台はプリセット名で引く）が普通である．これらは書き出しのときに Web UI が埋める

### 照明についての注意

- 照明はOUT0〜OUT7のチャネル番号で指定する．左右などの物理的な意味はボード側の配線で決まるため，レシピにはチャネル番号だけを書く
- 左右照明の切り替えレシピの例．ステップ1で右側のチャネルだけを点ける場合は，その配線でのマスク値を `light_out` に書く

## 4．継承の解決規則

各ステップの `values` は，次の規則で求めたものである．受け取り側が `overrides` から自分で求め直す必要はないが，求め直すときはこの規則に従う．

1. 土台：`base_values` があればそれ．無ければ `base_preset` 名で引き当てたプリセットの値（`base_preset` が `null`，または見つからなければ実行開始時点の現在値）
2. ステップの `overrides` で書かれた項目だけを上書きする
3. `light_out` が `null` でない場合だけ，照明マスクを適用する

## 5．条件マニフェスト（データセット側の保持形式）

撮影した写真については，データセット側で写真ごとに実効条件を保持する．継承を解決した最終値で記録するのが要点で，レシピ定義そのものではなく，実際に効いた条件を残す．

- ファイル名：写真と同名で拡張子だけ `.json` にしたもの（例：`露出5段_03_20261117-083012.jpg` に対して `露出5段_03_20261117-083012.json`）
- 構造：

```json
{
  "format": "esp32p4-cam/shot-manifest",
  "version": 1,
  "photo": "露出5段_03_20261117-083012.jpg",
  "run_id": "20261117-084500",
  "specimen_id": "XD4-2026-011",
  "recipe": "露出5段",
  "step_index": 3,
  "taken_at": "2026-11-17T08:30:12+09:00",
  "effective": {
    "exposure_mode": "manual",
    "exposure_us": 10000,
    "gain_mode": "manual",
    "gain_index": 10,
    "focus_mode": "manual",
    "focus_pos": 240,
    "wb_mode": "manual",
    "wb_red": 1600,
    "wb_blue": 1400,
    "sp": "B"
  },
  "light_out": 255,
  "q": 95
}
```

- `effective` は実効条件（カメラ9項目とsp）．送った値ではなく，撮影直前にボードから読み戻した値（ボードが範囲に丸めるため）
- レシピ外で撮影した写真（通常のシャッター）では，`run_id`，`specimen_id`，`recipe`，`step_index` を `null` とし，そのときの現在値を記録する

## 6．ラン計画（データセット側への事前申告）

マニフェストは撮影後の記録なので，データセット側は「今どの試験片を，どのレシピで，何枚，どの条件で撮影しているか」を事前に知ることができない．そのために，レシピ実行の開始時にラン計画を立てる．

- レシピは使い回す撮り方の定義，ラン計画はある試験片をあるレシピで1回撮るときの予定表と進捗である
- 実行開始時に，操作者が試験片IDを入力する（レシピタブの実行画面で指定する）
- ラン計画は，有効なステップごとに継承を解決した実効条件まで含む．データセット側はこれで，受信すべき写真の枚数と条件を事前に把握し，来ない写真（欠け）を検出できる

```json
{
  "format": "esp32p4-cam/run-plan",
  "version": 1,
  "run_id": "20261117-084500",
  "specimen_id": "XD4-2026-011",
  "recipe_name": "露出5段",
  "started_at": "2026-11-17T08:45:00+09:00",
  "status": "running",
  "expected_count": 5,
  "done_count": 0,
  "steps": [
    {
      "step_index": 1,
      "effective": {
        "exposure_mode": "manual",
        "exposure_us": 2500,
        "gain_mode": "manual",
        "gain_index": 10,
        "focus_mode": "manual",
        "focus_pos": 240,
        "wb_mode": "manual",
        "wb_red": 1600,
        "wb_blue": 1400,
        "sp": "B"
      },
      "light_out": 255,
      "q": 95
    }
  ]
}
```

- `run_id` は実行ごとに一意（開始時刻ベース．`YYYYMMDD-HHMMSS`）
- `step_index` はレシピ内のステップ番号（1始まり）．無効なステップは含めないので，番号が飛ぶことがある．写真のファイル名の `NN` と同じ番号
- ラン計画の `effective` は予定値（土台と `overrides` から解決した値）．ボードが丸めた実際の値は，マニフェストの `effective` に入る
- `status` は `running`，`done`，`stopped`（中断），`error`（全体が続けられなかった）のいずれか．`done_count` は終わったステップ数（失敗したステップも数える）．終了時に最終状態へ更新される
- マニフェスト（第5節）にも `run_id` と `specimen_id` を含め，写真とラン計画を対応付けられるようにする

取得と登録のエンドポイントは第7節に置く．

## 7．取得エンドポイント

ボードから取得する際の対応は次の通り．usb-app ではPCサーバーが同じパスで中継する．

- プリセット全件：`GET /presets`．保存，適用，削除，改名は `POST /presets/save`，`POST /presets/load`，`POST /presets/delete`，`POST /presets/rename`
- レシピ全件：`GET /recipes`（`{"max": 20, "max_steps": 30, "recipes": [...]}`．要素はボード内部の保存形で，`base_values` と `values` は持たないことが多い）
- レシピ個別：`GET /recipes?name=<名前>`．指定した1件のレシピオブジェクトを返す．無い名前には 404 `{"error": ...}` を返す
- ラン計画の取得：`GET /run`．`{"run": <ラン計画 または null>, "age_ms": <最後に登録・更新されてからのミリ秒 または null>}` を返す．データセット側はこれをポーリングして，撮影の進行と到着予定を把握する．`status` が `running` のまま `age_ms` が大きいときは，実行役が止まったとみなせる
- ラン計画の登録：`POST /run`（ボディにラン計画）．実行開始時に1回だけ送る．ボードは直近1件だけを RAM に持ち，前のラン計画と置き換える（再起動で消える）
- ラン計画の更新：`POST /run/update?run_id=…&done_count=…&status=…`（どちらか一方でもよい）．ステップが終わるたびに `done_count` を，終了時に `status` を送る．`run_id` が保持中のラン計画と違えば 409
- バンドルの書き出しは専用エンドポイントを持たず，Web UIが `GET /presets`，`GET /recipes`，`GET /ctrl` の応答から，第1節のバンドル形式でファイルにする

## 8．版管理

- 形式を変えるときは `version` を上げる．後方互換のない変更は，受け取り側が黙って誤読しないよう，必ず版を上げること
- 未知のキーは無視してよい（前方互換）．ただしプリセットとレシピの既定の意味は変えない
