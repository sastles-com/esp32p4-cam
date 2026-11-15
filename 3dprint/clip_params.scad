// 薄型センタリングクリップの共通寸法（centering_clip.scad から include する）
// 座標: 裏板上面 Z=0（裏板は Z<0）、中心 = ピニオン軸、X = 挟む方向（試験片の幅方向）、Y = 試験片の長辺方向
// 右キャリアの顎面位置を J（中心からの距離）で表す。左は Z 軸まわり 180 度回転で同形。

/* [試験片・顎] */
W_max = 40;              // 対応する最大幅
J_min = 3;               // 顎面の閉じストッパ位置（幅 6mm 相当）
J_max = 24;              // 顎面の開きストッパ位置（幅 48mm 相当）
jaw_h = 7;               // 支持面からの顎の高さ（試験片厚さ 5mm を当たり面 5.5mm で受ける）
jaw_len = 36;            // 顎の当たり面の長さ（Y）
jaw_t = 4;               // 顎の厚さ（X）
lead = 1.5;              // 顎上端の 45 度導入面の大きさ
pad_t = 1.0;             // 当たり面パッド（TPU）の厚さ。パッドなしは 0
edge_recess = 2;         // 顎以外の範囲でキャリア端面を顎面から下げる量

/* [ベース] */
base_x = 130;            // 外形（X）
base_y = 104;            // 外形（Y）
base_t = 3.5;            // 厚さ = 支持面の高さ
roof_t = 0.8;            // ポケット・トンネル上の天井厚さ（印刷時はベッド側）
base_r = 4;              // 外形の角R
mount_pts = [[-55, -46], [55, -46], [-55, 46], [55, 46], [0, -46], [0, 46]];
mount_d = 3.4;           // M3 皿ねじ 通し穴
csk_d = 7.2;             // 皿もみ径（ねじ頭が支持面より約 0.3mm 沈む。キャリアがこの上を通る）
mount_slot = 6;          // 床の穴位置に合わせる長穴の長さ（X）

/* [裏板] */
// ベース裏の掘り込みを塞ぎ、ピニオンとラックの抜けを止める薄板（Z = -plate_t〜0）
plate_t = 1.5;           // 厚さ（M2 皿ねじの頭が沈む最小限）
plate_screw_pts = [[-40, -25], [40, -25], [-40, 25], [40, 25], [0, -25], [0, 25]];
plate_hole_d = 2.3;      // M2 通し穴
plate_csk_d = 4.2;       // M2 皿もみ径（床側）
plate_pilot_d = 1.7;     // ベース側 M2 タッピン下穴
plate_pilot_depth = 2.5; // ベース側 下穴深さ（支持面まで貫通させない）

/* [歯車] */
m = 1.5;                 // モジュール
z = 10;                  // ピニオン歯数
pa = 25;                 // 圧力角（20 度より切り下げが小さい）
bl_p = 0.1;              // ピニオン歯厚の減らし量（ピッチ線上）
bl_r = 0.1;              // ラック歯厚の減らし量（ピッチ線上）
ha_r = 1.3;              // ラック歯末のたけ（干渉限界 r·sin²α ≈ 1.34 未満）
pinion_t = 2.4;          // ピニオン厚さ（床に置く、Z 0〜pinion_t）
boss_d = 5;              // ベース天井から下がる軸ボス径（中心の基準）
bore_clr = 0.2;          // ピニオン穴とボスのすき間（半径）
pocket_clr = 0.5;        // ピニオン歯先とポケット壁のすき間

/* [ラック] */
rack_teeth_len = 36;     // 歯の付いた長さ
rack_z0 = 0.3;           // ラック下面の床からの高さ
roof_gap = 0.2;          // ラック上面と天井のすき間
rack_back = 2.4;         // 歯底から背面までの厚さ
leg_len = 8;             // 立ち上がり脚の長さ（X）
slide_clr = 0.25;        // トンネル・スロットとラックのすき間（片側）
stop_clr = 0.2;          // スロット端（ストッパ）の余裕

/* [キャリア] */
car_t = 2.5;             // 板厚
car_len = 30;            // 顎面から外側への長さ
car_half_y = 47;         // Y 方向の半幅
screw_pilot_d = 1.7;     // M2 タッピンねじ下穴（ラック脚側）
screw_hole_d = 2.4;      // M2 通し穴（キャリア側）
hook_u = 19;             // 輪ゴムフックの顎面からの距離
hook_y = 44;             // 輪ゴムフックの中心からの距離（Y）
hook_d = 4;              // フック軸径
hook_h = 3;              // フック軸高さ（キャリア上面から）
cap_d = 7;               // 抜け止めつば径
cap_t = 0.8;             // つば厚さ

// ---- 導出値 ----
r = m * z / 2;                       // ピッチ円半径
p = PI * m;                          // ピッチ
ra = r + m;                          // 歯先円半径
travel = J_max - J_min;              // 顎 1 個の移動量
mesh_margin = (rack_teeth_len - travel) / 2;
tooth_lo = -J_max - mesh_margin;     // ラック局所座標 u = x - J での歯の範囲
tooth_hi = -J_min + mesh_margin;
leg_u0 = W_max / 2 + 1 - J_min;      // 脚の開始位置（スロットが試験片の外に出る）
bar_lo = tooth_lo - p / 2;           // ラック棒の内側端
bar_hi = leg_u0 + leg_len;           // ラック棒の外側端
y_tip = r - ha_r;                    // 右ラックの歯先線
y_root = r + 1.25 * m;               // 右ラックの歯底線
y_back = y_root + rack_back;         // 右ラックの背面
rack_h = base_t - roof_t - roof_gap - rack_z0;
screw_y = (y_tip + y_back) / 2;
screw_u = [leg_u0 + 2, leg_u0 + leg_len - 2];
pocket_r = ra + pocket_clr;

assert(rack_teeth_len > travel, "rack_teeth_len must exceed travel");
assert(J_min + leg_u0 - stop_clr > W_max / 2, "slot must stay outside the specimen");
assert(pinion_t < base_t - roof_t, "pinion must fit under the roof");
assert(ha_r < r * pow(sin(pa), 2), "rack addendum interferes with pinion");
assert(J_max + car_len < base_x / 2, "carrier overhangs the base");
