// バー端部の差し込みソケット（別体、底面を相手部品に接着）
// フレーム取付ジグ・カメラトレイの両方で共用する。

include <rack_params.scad>

/* [ソケット] */
fit = 0.2;               // バーとのすき間（片側）
sock_wall = 2.5;         // 側壁厚
sock_floor = 2;          // 底厚（接着面）
sock_depth = end_flat;   // 差し込み深さ = 歯なし区間

$fn = 64;

in_x = base_t + 2 * fit;     // バー厚さ方向
in_y = W + 2 * fit;          // バー幅方向
sock_x = in_x + 2 * sock_wall;
sock_y = in_y + 2 * sock_wall;

module bar_socket() {
    difference() {
        cube([sock_x, sock_y, sock_floor + sock_depth]);
        translate([sock_wall, sock_wall, sock_floor])
            cube([in_x, in_y, sock_depth + 1]);
    }
}

bar_socket();
