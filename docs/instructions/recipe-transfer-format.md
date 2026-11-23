# レシピ転送フォーマット仕様 v1

esp32p4-cam（wifi-app／usb-app共通）．2026年11月17日制定．

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

名前付きの撮影条件の集合．キーは `/ctrl` の項目名と同一とする．

```json
{
  "name": "標準_屋内",
  "values": {
    "exposure_mode": 0,
    "exposure_us": 10000,
    "gain_mode": 0,
    "gain_index": 10,
    "focus_mode": 0,
    "focus_pos": 240,
    "wb_mode": 0,
    "wb_red": 1.60,
    "wb_blue": 1.40,
    "sp": 0,
    "shot_q": 95,
    "shot_light": 1
  }
}
```

- 項目は12項目．カメラ条件9項目（exposure_mode，exposure_us，gain_mode，gain_index，focus_mode，focus_pos，wb_mode，wb_red，wb_blue）に，sp，shot_q（撮影画質），shot_light（撮影時の照明一括ON/OFF）を加えたもの
- **向き（hflip，vflip）は含めない**．2026年11月16日の決定による．適用で再起動が起きないようにするためである
- 各項目の値域はボードの `GET /ctrl/caps` が返す範囲に従う．この文書では値域を固定しない
- 一部の項目だけを持つプリセットも許す．その場合，無い項目は適用時に変更しない

## 3．レシピ

複数条件の連続撮影の手順．ベースとなる条件に，ステップごとの上書きを重ねて実行する．

```json
{
  "name": "露出5段",
  "base_preset": "標準_屋内",
  "base_values": {
    "exposure_mode": 0,
    "exposure_us": 10000,
    "gain_mode": 0,
    "gain_index": 10,
    "focus_mode": 0,
    "focus_pos": 240,
    "wb_mode": 0,
    "wb_red": 1.60,
    "wb_blue": 1.40,
    "sp": 0,
    "shot_q": 95,
    "shot_light": 1
  },
  "steps": [
    { "enabled": true,  "overrides": { "exposure_us": 2500 },  "light_out": 255, "q": 95, "settle_ms": 800 },
    { "enabled": true,  "overrides": { "exposure_us": 5000 },  "light_out": 255, "q": 95, "settle_ms": 800 },
    { "enabled": true,  "overrides": { "exposure_us": 10000 }, "light_out": 255, "q": 95, "settle_ms": 800 },
    { "enabled": true,  "overrides": { "exposure_us": 20000 }, "light_out": 255, "q": 95, "settle_ms": 800 },
    { "enabled": true,  "overrides": { "exposure_us": 40000 }, "light_out": 255, "q": 95, "settle_ms": 800 }
  ]
}
```

### フィールドの意味

- `base_preset`：土台にするプリセット名．`null` の場合は，実行開始時点の現在の設定を土台にする
- `base_values`：土台プリセットの値の控え（スナップショット）．レシピを書き出すときに，その時点のプリセットの内容を埋め込む．**再現性のための項目で，転送ファイルでは必須**とする．受け取り側の同名プリセットが異なる内容でも，同じレシピは同じ条件で実行されなければならない．実行時は `base_values` があればそれを土台とし，`base_preset` は由来を示す情報として扱う．`base_preset` が `null` のときは `base_values` も持たない
- 転送ファイルの `base_values` は，**12項目すべてを含む**こと．これにより，レシピファイルだけで全パラメータが確定する（プリセットを別に取得しなくてよい）．ボード内部の保存では部分項目のプリセットも扱えるが，書き出しのときに現在値で補完して12項目そろえる
- `steps`：実行順の配列．各ステップは次を持つ
  - `enabled`：falseのステップは実行しない（定義は残る）
  - `overrides`：土台に対して上書きする項目だけを書く．キーはカメラ9項目（exposure_mode，exposure_us，gain_mode，gain_index，focus_mode，focus_pos，wb_mode，wb_red，wb_blue）のうちの任意の部分集合．**書かれていない項目は土台を継承する**．sp はステップでは変更せず，土台の値を使う．shot_q と shot_light はステップ専用の `q` と `light_out` で指定する
  - `light_out`：照明出力マスク（0〜255）．ビットがOUT0〜OUT7に対応する．`null` の場合は照明を変更しない
  - `q`：JPEG画質．省略時は95
  - `settle_ms`：条件適用後に撮影するまでの待機時間（ミリ秒）．省略時は800

### 照明についての注意

- 照明はOUT0〜OUT7のチャネル番号で指定する．左右などの物理的な意味はボード側の配線で決まるため，レシピにはチャネル番号だけを書く
- 左右照明の切り替えレシピの例．ステップ1で右側のチャネルだけを点ける場合は，その配線でのマスク値を `light_out` に書く

## 4．継承の解決規則

実行時の実効条件は，次の順で決まる．

1. `base_values` があればそれを土台とする．無ければ `base_preset` 名で引き当てたプリセットの値を土台とする（どちらも無い，または `base_preset` が `null` の場合は実行開始時点の現在値）
2. ステップの `overrides` で書かれた項目だけを上書きする
3. `light_out` が `null` でない場合だけ，照明マスクを適用する

受け取り側がレシピを読むときも，この規則で各ステップの実効条件を再現できる．

## 5．条件マニフェスト（データセット側の保持形式）

レシピ実行で得た写真については，データセット側で写真ごとに実効条件を保持する．継承を解決した最終値で記録するのが要点で，レシピ定義そのものではなく，実際に効いた条件を残す．

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
    "exposure_mode": 0,
    "exposure_us": 10000,
    "gain_mode": 0,
    "gain_index": 10,
    "focus_mode": 0,
    "focus_pos": 240,
    "wb_mode": 0,
    "wb_red": 1.60,
    "wb_blue": 1.40,
    "sp": 0
  },
  "light_out": 255,
  "q": 95
}
```

- `effective` は実効条件（カメラ9項目とsp）．プリセット単独で撮影した写真については，`recipe` を `null`，`step_index` を `null` とし，そのときの現在値を記録する

## 6．ラン計画（データセット側への事前申告）

マニフェストは撮影後の記録なので，データセット側は「今どの試験片を，どのレシピで，何枚，どの条件で撮影しているか」を事前に知ることができない．そのために，レシピ実行の開始時にラン計画を立てる．

- 実行開始時に，操作者が試験片IDを入力する（レシピタブの実行画面で指定する）
- ラン計画は，ステップごとの継承を解決した実効条件まで含む．データセット側はこれで，受信すべき写真の枚数と条件を事前に把握し，来ない写真（欠け）を検出できる

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
        "exposure_mode": 0,
        "exposure_us": 2500,
        "gain_mode": 0,
        "gain_index": 10,
        "focus_mode": 0,
        "focus_pos": 240,
        "wb_mode": 0,
        "wb_red": 1.60,
        "wb_blue": 1.40,
        "sp": 0
      },
      "light_out": 255,
      "q": 95
    }
  ]
}
```

- `run_id` は実行ごとに一意（開始時刻ベースでよい）
- `status` は `running`，`done`，`stopped`，`error` のいずれか．`done_count` は完了したステップ数．終了時に最終状態へ更新される
- マニフェスト（第5節）にも `run_id` と `specimen_id` を含め，写真とラン計画を対応付けられるようにする

取得と登録のエンドポイントは第7節に置く．

## 7．取得エンドポイント

ボードから取得する際の対応は次の通り．

- プリセット全件：`GET /presets`（実装済み）．保存，適用，削除，改名は `POST /presets/save`，`POST /presets/load`，`POST /presets/delete`，`POST /presets/rename`
- レシピ全件：`GET /recipes`（レシピ実装時にこの名前で作る．応答はバンドルの `recipes` 配列と同じ形）
- レシピ個別：`GET /recipes?name=<名前>`．指定した1件のレシピオブジェクトを返す．無い名前には `{"error": ...}` を返す
- ラン計画の取得：`GET /run`．現在実行中，または直近のラン計画（第6節）を返す．データセット側はこれをポーリングして，撮影の進行と到着予定を把握する
- ラン計画の登録と更新：`POST /run`．実行開始時に計画を登録し，進行と終了にあわせて `done_count` と `status` を更新する（登録先はボード側とし，実行役のブラウザJSが書き込む）
- バンドルの書き出しは専用エンドポイントを持たず，Web UIが `GET /presets` と `GET /recipes` の応答を束ねて，第1節のバンドル形式でファイルにする

## 8．版管理

- 形式を変えるときは `version` を上げる．後方互換のない変更は，受け取り側が黙って誤読しないよう，必ず版を上げること
- 未知のキーは無視してよい（前方互換）．ただしプリセットとレシピの既定の意味は変えない
