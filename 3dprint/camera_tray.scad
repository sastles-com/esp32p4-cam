// Raspberry Pi Camera Module 3 用トレイ
// カメラは上向きに搭載し、底面裏側をバーに接着する。

/* [基板] */
pcb_w = 25;              // 基板幅（X）
pcb_h = 24;              // 基板高さ（Y、FPC側が Y=0）
pcb_t = 1;               // 基板厚
hole_dx = 21;            // 取付穴 横間隔
hole_dy = 12.5;          // 取付穴 縦間隔
hole_edge = 2;           // 取付穴の基板端からの距離（左右・上辺）

/* [トレイ] */
fit_clr = 0.5;           // 基板と内壁のすき間（片側）
wall = 2;                // 壁厚
floor_t = 3;             // 底厚
standoff_h = 4;          // 柱高さ（底面上）
standoff_d = 4.5;        // 柱外径
tap_d = 1.8;             // M2 タッピング下穴
tap_floor_keep = 1;      // 下穴が底を抜けないよう残す厚さ
corner_r = 3;            // 外形の角R

/* [FPC 切り欠き] */
fpc_notch_w = 18;        // 16mm 幅 FPC 用

$fn = 64;

in_w = pcb_w + 2 * fit_clr;
in_h = pcb_h + 2 * fit_clr;
out_w = in_w + 2 * wall;
out_h = in_h + 2 * wall;
tray_h = floor_t + standoff_h + pcb_t;   // 壁上端 = 基板上面

// 基板原点（左下）のトレイ座標
pcb_x0 = wall + fit_clr;
pcb_y0 = wall + fit_clr;

// 取付穴（基板座標、上辺から hole_edge と hole_edge + hole_dy）
holes = [for (x = [hole_edge, hole_edge + hole_dx])
         for (y = [pcb_h - hole_edge, pcb_h - hole_edge - hole_dy]) [x, y]];

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
                translate([pcb_x0 + p[0], pcb_y0 + p[1], 0])
                    cylinder(d = standoff_d, h = floor_t + standoff_h);
        }
        for (p = holes)
            translate([pcb_x0 + p[0], pcb_y0 + p[1], tap_floor_keep])
                cylinder(d = tap_d, h = tray_h);
        // FPC 側（Y=0）の壁を底面高さまで切り欠く
        translate([pcb_x0 + pcb_w / 2 - fpc_notch_w / 2, -1, floor_t])
            cube([fpc_notch_w, wall + 2, tray_h]);
    }
}

tray();
