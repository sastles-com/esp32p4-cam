// バー端部の差し込みソケット（別体、底面を相手部品に接着）
// フレーム取付ジグ・カメラトレイの両方で共用する。
// バーの細い側面と向き合う 2 枚の壁をスナップ爪にし，返しがバー端の溝（rack_bar.scad の snap_groove）に
// パチッとはまる．溝の無い古いバーでも，爪が 0.4 押し付けて摩擦で保持する．
// 印刷: 底面をベッドに置いてそのまま．サポート不要．

include <rack_params.scad>

/* [ソケット] */
fit = snap_fit;          // バーとのすき間（片側）
sock_wall = 2.5;         // 側壁厚
sock_floor = 2;          // 底厚（接着面）
sock_depth = end_flat;   // 差し込み深さ = 歯なし区間

/* [スナップ爪] */
snap_enable = true;
tab_t = 1.5;             // 爪の厚さ（壁の外側を削って薄くする）
tab_w = 7;               // 爪の幅
slot_w = 0.8;            // 爪の両脇のスリット

$fn = 64;

in_x = base_t + 2 * fit;     // バー厚さ方向
in_y = W + 2 * fit;          // バー幅方向
sock_x = in_x + 2 * sock_wall;
sock_y = in_y + 2 * sock_wall;
sock_h = sock_floor + sock_depth;
tab_x0 = sock_x / 2 - tab_w / 2;

// 計算値
tab_l = sock_depth;
defl = snap_bead_d - fit;
strain = 3 * tab_t * defl / (2 * tab_l * tab_l);
echo(str("bar_socket: ", sock_x, " x ", sock_y, " x ", sock_h, "  tab L=", tab_l, " t=", tab_t,
         " deflection=", defl, " strain=", strain * 100, " %  engagement=", defl, " mm"));
if (strain > 0.02) echo("WARNING: snap strain > 2 %");
if (tab_x0 - slot_w < 1.5) echo("WARNING: socket corners too thin");

// 返しの断面（u = 壁の内面から内側へ，z = 高さ）．下面は水平（抜け止め），上面は 45°（導入）
module bead_profile() {
    z0 = sock_floor + snap_bead_x0;
    polygon([[-0.01, z0], [snap_bead_d, z0], [snap_bead_d, z0 + snap_bead_h - snap_bead_d],
             [-0.01, z0 + snap_bead_h]]);
}

module bar_socket() {
    difference() {
        union() {
            difference() {
                cube([sock_x, sock_y, sock_h]);
                translate([sock_wall, sock_wall, sock_floor])
                    cube([in_x, in_y, sock_depth + 1]);
            }
            if (snap_enable) {
                // y = sock_wall の壁（内側 +y へ）
                translate([tab_x0 + 0.3, sock_wall, 0]) rotate([90, 0, 90])
                    linear_extrude(tab_w - 0.6) bead_profile();
                // y = sock_y - sock_wall の壁（内側 -y へ）
                translate([tab_x0 + 0.3, sock_y - sock_wall, 0]) mirror([0, 1, 0]) rotate([90, 0, 90])
                    linear_extrude(tab_w - 0.6) bead_profile();
            }
        }
        if (snap_enable)
            for (s = [0, 1]) {
                y_out = s == 0 ? -1 : sock_y - (sock_wall - tab_t);
                // 爪の外側を削って tab_t にする
                translate([tab_x0, y_out, sock_floor])
                    cube([tab_w, sock_wall - tab_t + 1, sock_depth + 1]);
                // 爪の両脇のスリット
                for (x = [tab_x0 - slot_w, tab_x0 + tab_w])
                    translate([x, s == 0 ? -1 : sock_y - sock_wall - 0.01, sock_floor])
                        cube([slot_w, sock_wall + 1.01, sock_depth + 1]);
            }
    }
}

bar_socket();
