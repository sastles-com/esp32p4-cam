// Raspberry Pi Camera Module 3 用トレイ＋フタ
// カメラは上向きに搭載し，トレイ底面の裏をバーに接着する．
// 基板はねじを使わず，柱の先のピンを基板の取付穴に通して位置決めする．
// フタはトレイの外側にかぶせ，左右（±X）のスナップ爪がトレイ外壁の溝にパチッとはまる．
// フタの柱がピンを受けて基板を上から押さえ，カメラが落ちないようにする．
// フタ上面の穴の上に偏光フィルムを貼る（浅い座ぐりで位置決め）．
//
// 座標: トレイ左下の角が原点，X = 基板幅，Y = 基板高さ（FPC 側が Y=0），z=0 がトレイ底面．
// 偏光板の回転リング（pol_enable）: フタの穴に回転リング（rotor）の筒を通し，フタの内側から C 形の止め輪（ring）を
// 筒の溝にはめて抜け止めする．リング上面の座ぐりに偏光フィルム（円形）を貼る．リングの板バネ 2 枚がフタ上面を押し，
// 止め輪を天井の裏に押し付けて摩擦で角度を保つ．フタ上面に 0〜180° の目盛り（15° 毎），リングに指標（0° と 180°）．
// 出力: part = "tray" / "lid" / "rotor" / "ring" / "assembly"
//   tray: そのまま（底面をベッドに）印刷．
//   lid : 上下を反転して（天面をベッドに）印刷．スナップ爪の返しは 45° の面とわずかな張り出しだけでサポート不要．
//
// 要確認の仮定値:
//   lens_y  : レンズ中心の位置．試作の写真から FPC 側の端から 9.5（下側の取付穴の列と同じ高さ）とした
//   lid_clear: 基板上面からフタ内面までの高さ（指定値 8）．レンズの先端がこれより低いことを実物で確認すること．
//              データシートの全高 11.5（基板込み）だとレンズの先端は基板上面から約 10.5 で当たる．

/* [出力] */
part = "assembly";       // [tray, lid, rotor, ring, assembly]

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

/* [偏光板の回転リング] */
pol_enable = true;
pol_hole_r = 9.0;        // フタの軸受け穴の半径（下側の押さえ柱の縁 r=8.5 にかかる分は柱の付け根を削る）
pol_clr = 0.2;           // 軸受けのすき間（半径）
pol_ap_r = 7.5;          // 撮影の開口の半径
pol_R = 12.5;            // リングの外半径
pol_fl_t = 2.0;          // リングのつばの厚さ
pol_gap = 0.3;           // 組んだ状態のつば下面とフタ上面のすき間
pol_film_r = 9.2;        // 偏光フィルムの座ぐり（φ18 のフィルム用）
pol_film_depth = 0.4;
pol_groove_d = 0.5;      // 筒の止め輪溝の深さ
ring_t = 1.0;            // 止め輪の厚さ
ring_w = 1.5;            // 止め輪の幅（半径方向）
ring_open = 0.92;        // 止め輪の開口幅 / 溝底の直径（押し込むとき片側 0.66 開く）
tongue_ang = 40;         // 板バネの角度幅
tongue_t = 1.2;          // 板バネの厚さ
tongue_preload = 0.25;   // 板バネのたわみ
tick_r0 = 13.0;          // 目盛り（フタ上面）
tick_len = 1.0;
tick_len_major = 2.0;

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
// 回転リング（z はトレイ座標．組んだ状態＝板バネに押されて止め輪が天井に当たった状態）
tube_r = pol_hole_r - pol_clr;
gr_r = tube_r - pol_groove_d;               // 溝底の半径
fl_z0 = lid_top + pol_gap;                  // つば下面
gr_z0 = lid_top_in - ring_t;                // 溝の下面（止め輪の下面）
tube_z0 = gr_z0 - 0.8;                      // 筒の下端
ring_ro = gr_r + 0.05 + ring_w;             // 止め輪の外半径
post_notch_r = ring_ro + 0.4;               // 押さえ柱の付け根を削る半径

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
        if (pol_enable) {
            // 回転リングの軸受け穴
            translate([lens[0], lens[1], lid_top_in - 1]) cylinder(r = pol_hole_r, h = lid_t + 2);
            // 止め輪の逃げ（下側の押さえ柱の付け根を削る）
            translate([lens[0], lens[1], lid_top_in - ring_t - 1]) cylinder(r = post_notch_r, h = ring_t + 1.01);
            // 目盛り（0〜180°，15° 毎．0・45・90・135・180 は長い）．+X が 0°，+Y（FPC と反対側）が 90°
            for (a = [0 : 15 : 180])
                translate([lens[0], lens[1], lid_top - 0.4]) rotate([0, 0, a])
                    translate([tick_r0, -0.25, 0]) cube([a % 45 == 0 ? tick_len_major : tick_len, 0.5, 1]);
        } else
        // 撮影穴（外側へ 45° で広がる）
        translate([lens[0], lens[1], lid_top_in - 0.01])
            cylinder(d1 = view_d, d2 = view_d + 2 * (lid_t + 0.02), h = lid_t + 0.02);
        // 偏光フィルムの座ぐり
        if (film_w > 0)
            translate([lens[0] - film_w / 2, lens[1] - film_w / 2, lid_top - film_depth])
                cube([film_w, film_w, film_depth + 1]);
    }
}

// 回転リング（組んだ状態の位置，レンズ中心が原点）
module rotor() {
    tongue_cx = [120, 300];                 // 板バネの中心角（指標 0° から離す）
    difference() {
        union() {
            translate([0, 0, fl_z0]) cylinder(r = pol_R, h = pol_fl_t);
            translate([0, 0, tube_z0]) cylinder(r = tube_r, h = fl_z0 - tube_z0 + 0.01);
            // 板バネ先端の突起（フタ上面を押す）
            for (c = tongue_cx)
                rotate([0, 0, c + tongue_ang / 2 - 4]) translate([pol_R - 0.8, 0, fl_z0 - pol_gap - tongue_preload])
                    cylinder(d1 = 0.8, d2 = 1.4, h = pol_gap + tongue_preload + (pol_fl_t - tongue_t) + 0.01);
        }
        // 開口
        translate([0, 0, tube_z0 - 1]) cylinder(r = pol_ap_r, h = 20);
        // 偏光フィルムの座ぐり
        translate([0, 0, fl_z0 + pol_fl_t - pol_film_depth]) cylinder(r = pol_film_r, h = 1);
        // 止め輪の溝
        translate([0, 0, gr_z0]) difference() {
            cylinder(r = tube_r + 1, h = ring_t + 0.2);
            translate([0, 0, -1]) cylinder(r = gr_r, h = ring_t + 2.2);
        }
        // 板バネ: 外周の帯（r = pol_R-1.6 .. pol_R）を，内側のスリットと先端のスリットで切り離し，下面を薄くする
        for (c = tongue_cx) {
            a0 = c - tongue_ang / 2;
            translate([0, 0, fl_z0 - 1]) linear_extrude(pol_fl_t + 2) arc2d(pol_R - 2.1, pol_R - 1.6, a0, tongue_ang);
            translate([0, 0, fl_z0 - 1]) linear_extrude(pol_fl_t + 2) arc2d(pol_R - 2.1, pol_R + 1, a0 + tongue_ang, 3);
            translate([0, 0, fl_z0 - 1]) linear_extrude(1 + pol_fl_t - tongue_t) arc2d(pol_R - 1.6 - 0.01, pol_R + 1, a0 + 2, tongue_ang);
        }
        // 指標（0° と 180°）: 外周の V 切り欠きと上面の線
        for (a = [0, 180]) rotate([0, 0, a]) {
            translate([pol_R, 0, fl_z0 - 1]) rotate([0, 0, 45]) cube([1.2, 1.2, pol_fl_t + 2], center = true);
            translate([pol_film_r + 0.6, -0.25, fl_z0 + pol_fl_t - 0.4]) cube([pol_R - pol_film_r - 1.2, 0.5, 1]);
        }
        // 指掛けの刻み（外周）
        for (a = [7.5 : 15 : 360]) rotate([0, 0, a]) translate([pol_R + 0.1, 0, fl_z0 - 1]) cylinder(r = 0.5, h = pol_fl_t + 2, $fn = 12);
    }
}

// 扇形の帯（r0..r1，a0 から da 度）
module arc2d(r0, r1, a0, da) {
    n = max(2, ceil(da / 3));
    polygon(concat([for (i = [0 : n]) let (a = a0 + da * i / n) r1 * [cos(a), sin(a)]],
                   [for (i = [n : -1 : 0]) let (a = a0 + da * i / n) r0 * [cos(a), sin(a)]]));
}

// C 形の止め輪（筒の溝に横から押し込む）
module cring() {
    open_w = ring_open * 2 * gr_r;
    difference() {
        cylinder(r = ring_ro, h = ring_t);
        translate([0, 0, -1]) cylinder(r = gr_r + 0.05, h = ring_t + 2);
        // 開口（導入の面取り付き）
        translate([0, 0, -1]) linear_extrude(ring_t + 2)
            polygon([[-open_w / 2, 0], [open_w / 2, 0], [open_w / 2 + 1, -ring_ro - 1], [-open_w / 2 - 1, -ring_ro - 1]]);
    }
}

// 計算値（回転リング）
if (pol_enable) {
    tl = (pol_R - 0.8) * PI * (tongue_ang - 4) / 180;
    echo(str("polarizer rotor: R=", pol_R, " aperture d=", 2 * pol_ap_r, " film d<=", 2 * pol_film_r - 0.4,
             "  tube r=", tube_r, " groove r=", gr_r, "  ring r=", gr_r + 0.05, "..", ring_ro, " open=", ring_open * 2 * gr_r,
             "  tongue L=", tl, " strain=", 3 * tongue_t * tongue_preload / (2 * tl * tl) * 100, " %",
             "  rotor top z=", fl_z0 + pol_fl_t, "  post notch r=", post_notch_r));
    if (lens[1] - ring_ro < wall + 0.1) echo("WARNING: retaining ring hits the lid inner wall");
}

if (part == "tray") tray();
else if (part == "lid") translate([0, 0, lid_top]) mirror([0, 0, 1]) lid();   // 天面をベッドに
else if (part == "rotor") translate([0, 0, fl_z0 + pol_fl_t]) mirror([0, 0, 1]) rotor();   // 上面をベッドに
else if (part == "ring") cring();
else {
    color("Gold") tray();
    color("SkyBlue", 0.6) lid();
    if (pol_enable) translate([lens[0], lens[1], 0]) {
        color("DimGray") rotor();
        color("Tomato") translate([0, 0, gr_z0]) cring();
    }
}
