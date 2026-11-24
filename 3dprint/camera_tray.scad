// Raspberry Pi Camera Module 3 用トレイ＋フタ
// カメラは上向きに搭載し，トレイ底面の裏をバーに接着する．
// 基板はねじを使わず，柱の先のピンを基板の取付穴に通して位置決めする．
// フタはトレイの外側にかぶせ，左右（±X）のスナップ爪がトレイ外壁の溝にパチッとはまる．
// フタの柱がピンを受けて基板を上から押さえ，カメラが落ちないようにする．
// フタ上面の穴の上に偏光フィルムを貼る（浅い座ぐりで位置決め）．
//
// 座標: トレイ左下の角が原点，X = 基板幅，Y = 基板高さ（FPC 側が Y=0），z=0 がトレイ底面．
// 出力: part = "tray" / "lid" / "assembly"
//   tray: そのまま（底面をベッドに）印刷．
//   lid : 上下を反転して（天面をベッドに）印刷．スナップ爪の返しは 45° の面とわずかな張り出しだけでサポート不要．
//
// 要確認の仮定値:
//   lens_y  : レンズ中心の位置．試作の写真から FPC 側の端から 9.5（下側の取付穴の列と同じ高さ）とした
//   lid_clear: 基板上面からフタ内面までの高さ（指定値 8）．レンズの先端がこれより低いことを実物で確認すること．
//              データシートの全高 11.5（基板込み）だとレンズの先端は基板上面から約 10.5 で当たる．

/* [出力] */
part = "assembly";       // [tray, lid, assembly]

/* [基板] */
pcb_w = 25;              // 基板幅（X）
pcb_h = 24;              // 基板高さ（Y、FPC側が Y=0）
pcb_t = 1.0;             // 基板厚（実物 1.0）．壁の高さ・ピン・フタの高さがこれに連動する
hole_dx = 21;            // 取付穴 横間隔
hole_dy = 12.5;          // 取付穴 縦間隔
hole_edge = 2;           // 取付穴の基板端からの距離（左右・上辺）
lens_y = 9.5;            // レンズ中心の y（基板座標，FPC 側の端から）．試作で仮定値 14.54 から FPC 側へ約 5mm ずれていたので修正

/* [トレイ] */
fit_clr = 0.5;           // 基板と内壁のすき間（片側）
wall = 2;                // 壁厚
floor_t = 3;             // 底厚
standoff_h = 4;          // 柱高さ（底面上）
standoff_d = 4.5;        // 柱外径
corner_r = 3;            // 外形の角R

/* [ピン（ねじの代わり）] */
pin_d = 1.9;             // 基板の取付穴 φ2.2（M2 用）に通す
pin_above = 1.0;         // 基板上面からのピンの突き出し
pin_chamfer = 0.3;       // 先端の面取り（入れやすくする）

/* [FPC 切り欠き] */
fpc_notch_w = 18;        // 16mm 幅 FPC 用

/* [スナップ] */
groove_z0 = 1.0;         // トレイ外壁の溝の下端
groove_z1 = 2.8;         // 溝の上端（爪の上面との遊び 0.2）
groove_d = 0.8;          // 溝の深さ（壁厚 2 に対し 1.2 残る）
tab_w = 12;              // 爪の幅（Y 方向）
slot_w = 0.8;            // 爪の両脇のスリット幅
slot_top = 10;           // スリットの上端（爪の根元）．爪の長さ = slot_top - lid_z0
bead_d = 0.7;            // 爪の返しの張り出し（内側へ）
bead_z1 = 2.6;           // 返しの上面（溝にかかる面，水平）

/* [フタ] */
lid_fit = 0.2;           // フタ内面とトレイ外面のすき間（片側）
skirt_t = 1.6;           // フタ側面の厚さ
lid_z0 = 1.0;            // フタ下端の高さ（トレイ底より上．接着面に当たらない）
lid_clear = 8;           // 基板上面からフタ内面（天井）までの高さ
lid_t = 1.5;             // 天板の厚さ
post_d = 4;              // 押さえ柱の外径
post_gap = 0.1;          // 押さえ柱の先端と基板上面のすき間
view_d = 14;             // 撮影穴の内側の直径（外側へ 45° で広がる）
film_w = 0;              // 偏光フィルム用の座ぐり（正方形）．0 で無し（試作で不要とした）
film_depth = 0.3;

$fn = 64;

in_w = pcb_w + 2 * fit_clr;
in_h = pcb_h + 2 * fit_clr;
out_w = in_w + 2 * wall;
out_h = in_h + 2 * wall;
tray_h = floor_t + standoff_h + pcb_t;   // 壁上端 = 基板上面
z_pcb = floor_t + standoff_h;            // 基板下面

// 基板原点（左下）のトレイ座標
pcb_x0 = wall + fit_clr;
pcb_y0 = wall + fit_clr;

// 取付穴（基板座標、上辺から hole_edge と hole_edge + hole_dy）
holes = [for (x = [hole_edge, hole_edge + hole_dx])
         for (y = [pcb_h - hole_edge, pcb_h - hole_edge - hole_dy]) [x, y]];

// フタ
lo = lid_fit + skirt_t;                  // フタ外形のトレイ外形からの張り出し
lid_top_in = tray_h + lid_clear;         // 天井（内面）
lid_top = lid_top_in + lid_t;            // 天面
lens = [pcb_x0 + pcb_w / 2, pcb_y0 + lens_y];
tab_y0 = out_h / 2 - tab_w / 2;

// 計算値
tab_l = slot_top - lid_z0;
strain = 3 * skirt_t * (bead_d - lid_fit) / (2 * tab_l * tab_l);
echo(str("lid: outer ", out_w + 2 * lo, " x ", out_h + 2 * lo, " x ", lid_top - lid_z0,
         "  top z=", lid_top, "  snap tab L=", tab_l, " deflection=", bead_d - lid_fit,
         " strain=", strain * 100, " %"));
if (strain > 0.02) echo("WARNING: snap strain > 2 %");
if (bead_d >= lid_fit + groove_d) echo("WARNING: bead deeper than groove");

module rounded_rect(w, h, r) {
    offset(r = r) offset(delta = -r) square([w, h]);
}

module tray() {
    difference() {
        union() {
            difference() {
                linear_extrude(tray_h) rounded_rect(out_w, out_h, corner_r);
                translate([wall, wall, floor_t])
                    linear_extrude(tray_h)
                        rounded_rect(in_w, in_h, max(corner_r - wall, 0.01));
            }
            for (p = holes)
                translate([pcb_x0 + p[0], pcb_y0 + p[1], 0]) {
                    cylinder(d = standoff_d, h = z_pcb);
                    // ピン: 基板の穴を通って pin_above だけ突き出す
                    cylinder(d = pin_d, h = tray_h + pin_above - pin_chamfer);
                    translate([0, 0, tray_h + pin_above - pin_chamfer])
                        cylinder(d1 = pin_d, d2 = pin_d - 2 * pin_chamfer, h = pin_chamfer);
                }
        }
        // FPC 側（Y=0）の壁を底面高さまで切り欠く
        translate([pcb_x0 + pcb_w / 2 - fpc_notch_w / 2, -1, floor_t])
            cube([fpc_notch_w, wall + 2, tray_h]);
        // スナップの溝（±X 外壁）
        for (x = [-1, out_w - groove_d])
            translate([x, tab_y0 - 1, groove_z0])
                cube([groove_d + 1, tab_w + 2, groove_z1 - groove_z0]);
    }
}

module lid() {
    difference() {
        union() {
            difference() {
                // 外形
                translate([-lo, -lo, lid_z0])
                    linear_extrude(lid_top - lid_z0) rounded_rect(out_w + 2 * lo, out_h + 2 * lo, corner_r + lo);
                // トレイにかぶさる部分（トレイ上端まで）
                translate([-lid_fit, -lid_fit, lid_z0 - 1])
                    linear_extrude(tray_h - lid_z0 + 1)
                        rounded_rect(out_w + 2 * lid_fit, out_h + 2 * lid_fit, corner_r + lid_fit);
                // 上の空間（内側はトレイの内壁と同じ．縁がトレイの壁の上面に載る）
                translate([wall, wall, tray_h - 0.01])
                    linear_extrude(lid_top_in - tray_h + 0.01)
                        rounded_rect(in_w, in_h, max(corner_r - wall, 0.01));
                // FPC の出口（Y=0 側，トレイの切り欠きと同じ幅）
                translate([pcb_x0 + pcb_w / 2 - fpc_notch_w / 2, -lo - 1, lid_z0 - 1])
                    cube([fpc_notch_w, lo + wall + 1, tray_h - lid_z0 + 1]);
                // スナップ爪の両脇のスリット
                for (x = [-lo - 1, out_w + lid_fit - 0.01], y = [tab_y0 - slot_w, tab_y0 + tab_w])
                    translate([x, y, lid_z0 - 1])
                        cube([skirt_t + 1.01, slot_w, slot_top - lid_z0 + 1]);
                // 爪の内側の逃げ（トレイ上端より上で，爪と縁を切り離す．縁は x >= 0.4 でトレイの壁に載る）
                for (x = [-lid_fit - 0.01, out_w - 0.4])
                    translate([x, tab_y0 - slot_w, tray_h - 0.01])
                        cube([lid_fit + 0.41, tab_w + 2 * slot_w, slot_top - tray_h + 0.01]);
            }
            // 押さえ柱（ピンを受け，基板を上から押さえる）
            for (p = holes)
                translate([pcb_x0 + p[0], pcb_y0 + p[1], tray_h + post_gap])
                    cylinder(d = post_d, h = lid_top_in - tray_h - post_gap + 0.01);
            // スナップ爪の返し（下面 45° の導入面，上面は水平）
            for (s = [0, 1])
                translate([s == 0 ? -lid_fit : out_w + lid_fit, tab_y0 + 0.5, 0])
                    mirror([s, 0, 0])
                        rotate([90, 0, 0]) mirror([0, 0, 1])
                            linear_extrude(tab_w - 1)
                                polygon([[-0.01, lid_z0], [bead_d, lid_z0 + bead_d], [bead_d, bead_z1], [-0.01, bead_z1]]);
        }
        // ピンの受け穴
        for (p = holes)
            translate([pcb_x0 + p[0], pcb_y0 + p[1], tray_h])
                cylinder(d = pin_d + 0.4, h = pin_above + 0.3);
        // 撮影穴（外側へ 45° で広がる）
        translate([lens[0], lens[1], lid_top_in - 0.01])
            cylinder(d1 = view_d, d2 = view_d + 2 * (lid_t + 0.02), h = lid_t + 0.02);
        // 偏光フィルムの座ぐり
        if (film_w > 0)
            translate([lens[0] - film_w / 2, lens[1] - film_w / 2, lid_top - film_depth])
                cube([film_w, film_w, film_depth + 1]);
    }
}

if (part == "tray") tray();
else if (part == "lid") translate([0, 0, lid_top]) mirror([0, 0, 1]) lid();   // 天面をベッドに
else {
    color("Gold") tray();
    color("SkyBlue", 0.6) lid();
}
