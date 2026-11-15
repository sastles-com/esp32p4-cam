// 噛み合わせ式 長さ調整バー（A: 丸穴 / B: スロット）
// 2本の歯面を向かい合わせて噛み合わせ、M5ボルトで固定する。

/* [出力] */
part = "A";              // "A" = 丸穴, "B" = スロット, "pair" = 噛み合わせ確認
print_on_side = true;    // true: 歯形断面をXY平面に置く（横置き印刷）

include <rack_params.scad>

/* [穴] */
hole_d = 5.5;            // M5 通し穴
cb_d = 10;               // 座ぐり径
cb_depth = 2;            // 座ぐり深さ（背面側）
a_hole_x = 100;          // A の丸穴位置（端からの距離）
b_slot_margin = 20;      // B のスロット両端の残し長さ

$fn = 64;

n_teeth = floor((L - 2 * end_flat) / pitch);

assert(root_r < tip_r, "root_r must be smaller than tip_r");

// 歯形断面（X: 長さ方向, Y: 厚さ方向、背面が Y=0）
module profile_2d() {
    dx = c / cos(half);
    dh = c / sin(half);
    offset(r = tip_r) offset(delta = -tip_r)       // 山・外角を丸める
    offset(r = -root_r) offset(delta = root_r)     // 谷を丸める
    union() {
        square([L, base_t]);
        for (i = [0 : n_teeth - 1]) {
            x0 = end_flat + i * pitch;
            polygon([
                [x0 + dx, base_t - 0.01],
                [x0 + pitch - dx, base_t - 0.01],
                [x0 + pitch / 2, base_t + tooth_h - dh]
            ]);
        }
    }
}

// Y 方向に貫通する穴（中心が x0..x1 の長穴、x0 == x1 で丸穴）
module through_slot(x0, x1) {
    hull() for (x = [x0, x1])
        translate([x, -1, W / 2]) rotate([-90, 0, 0])
            cylinder(d = hole_d, h = base_t + tooth_h + 2);
    hull() for (x = [x0, x1])
        translate([x, -1, W / 2]) rotate([-90, 0, 0])
            cylinder(d = cb_d, h = cb_depth + 1);
}

module bar(kind) {
    difference() {
        linear_extrude(W) profile_2d();
        if (kind == "A")
            through_slot(a_hole_x, a_hole_x);
        else
            through_slot(b_slot_margin + hole_d / 2, L - b_slot_margin - hole_d / 2);
    }
}

// 歯面を上にして平置き（X: 長さ, Y: 幅, Z: 厚さ）
module bar_flat(kind) {
    rotate([90, 0, 0]) translate([0, 0, -W]) bar(kind);
}

if (part == "pair") {
    bar_flat("B");
    // A を裏返して B の上に重ね、穴位置を合わせる
    color("orange")
    translate([0, 0, stack_h]) mirror([0, 0, 1])
    translate([pitch / 2, 0, 0]) bar_flat("A");
} else if (print_on_side) {
    bar(part);
} else {
    bar_flat(part);
}
