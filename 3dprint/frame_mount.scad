// 2020 アルミフレーム梁の下面に取り付けるプレート
// 下面中央に bar_socket を接着し、バーを梁から真下に吊り下げる。

/* [プレート] */
plate_l = 44;            // 梁の長手方向（座ぐり外側に 2mm 残す）
plate_w = 26;            // 梁と直交方向（bar_socket の幅 25.4 を受ける）
plate_t = 9;             // 厚さ

/* [2020 溝] */
key_w = 5.6;             // 溝幅 6mm に入る突起
key_h = 1.5;             // 突起高さ
key_l = 20;              // 突起長さ（ボルト穴の間に収める）

/* [ボルト] */
bolt_pitch = 30;         // M5 ボルト間隔
hole_d = 5.5;            // M5 通し穴
cb_d = 10;               // 座ぐり径
cb_depth = 5.5;          // 座ぐり深さ（キャップボルト頭が沈む）

$fn = 64;

// 座標: 下面（接着面・座ぐり側）が Z=0、梁に接する面が Z=plate_t
module frame_mount() {
    difference() {
        union() {
            translate([-plate_l / 2, -plate_w / 2, 0])
                cube([plate_l, plate_w, plate_t]);
            translate([-key_l / 2, -key_w / 2, plate_t - 0.01])
                cube([key_l, key_w, key_h + 0.01]);
        }
        for (x = [-bolt_pitch / 2, bolt_pitch / 2]) {
            translate([x, 0, -2])
                cylinder(d = hole_d, h = plate_t + key_h + 4);
            translate([x, 0, -1])
                cylinder(d = cb_d, h = cb_depth + 1);
        }
    }
}

frame_mount();
