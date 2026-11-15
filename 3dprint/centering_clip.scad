// 薄型センタリングクリップ（設計案 v1 を修正した v2）
// ピニオン 1 個に左右ラックを前後から噛ませ、両顎を常に中心対称に動かす。
// 輪ゴムは締め付け力だけを担い、中心位置はベースの軸ボスが決める。
//
// 部品: base ×1, plate ×1（裏板）, pinion ×1, rack ×2（同形）, carrier ×2（同形）, pad ×2（TPU）
// 金物: M3 皿ねじ ×6（床固定）, M2 皿タッピンねじ ×6（裏板）, M2 タッピンねじ ×4（キャリア⇔ラック）, 輪ゴム ×2
//
// 組立: ベースを裏返し、ピニオンをボスに、ラックをトンネルに入れる（両顎とも閉じ位置で噛ませる）。
//       裏返したまま裏板を M2 皿ねじで留め（ピニオン・ラックの抜け止め）、表に返す。
//       キャリアを載せ、M2 ねじでラック脚に固定。最後に裏板ごと床へ M3 皿ねじで留める。

/* [出力] */
part = "assembly";       // base / pinion / rack / carrier / pad / assembly / check2d / check3d
W = 25;                  // assembly・check での試験片幅
spec_L = 55;             // assembly での試験片長さ（表示のみ）
spec_t = 1;              // assembly での試験片厚さ（表示のみ）

include <clip_params.scad>

$fn = 64;

// 顎面位置 J と、そのときのピニオン回転角（度、時計回りが負）
function jaw_pos(w) = max(J_min, min(J_max, w / 2));
function pinion_angle(j) = -(j - J_min) / r * 180 / PI;

// ---------------- ピニオン ----------------
// 創成法: 創成用ラック歯を転がして歯溝を削り出す（切り下げも自動で反映される）
module tool_tooth() {               // ピッチ線 x = r、歯先が -X（ギヤ中心）向き
    s = p / 2 + bl_p;
    ha = 1.25 * m;
    hb = 1.6 * m;
    t = tan(pa);
    polygon([[r - ha, -(s / 2 - ha * t)], [r - ha, s / 2 - ha * t],
             [r + hb, s / 2 + hb * t], [r + hb, -(s / 2 + hb * t)]]);
}

module tooth_space() {              // +X 方向を中心とする歯溝 1 個
    for (phi = [-60 : 1 : 60])
        rotate(-phi) translate([0, r * phi * PI / 180]) tool_tooth();
}

// θ = 0 で歯が +Y と -Y を向く（両ラックが閉じ位置のときの噛み合い位相）
module pinion_2d() {
    difference() {
        circle(r = ra, $fn = 120);
        for (k = [0 : z - 1])
            rotate(90 + 180 / z + k * 360 / z) tooth_space();
        circle(d = boss_d + 2 * bore_clr);
    }
}

module pinion() {
    linear_extrude(pinion_t) pinion_2d();
}

// ---------------- ラック ----------------
// 右ラックの局所座標（u = x - J, y）。歯は -Y（ピニオン）向き。
module rack_2d() {
    s = p / 2 - bl_r;
    t = tan(pa);
    hf = 1.25 * m;
    union() {
        translate([bar_lo, y_root - 0.01]) square([bar_hi - bar_lo, y_back - y_root + 0.01]);
        translate([leg_u0, y_tip]) square([leg_len, y_back - y_tip]);
        // J = J_min で u = -J_min（x = 0）が歯溝の中心
        for (k = [-20 : 20]) {
            uc = -J_min + p / 2 + k * p;
            if (uc >= tooth_lo && uc <= tooth_hi)
                translate([uc, 0]) polygon([
                    [-(s / 2 - ha_r * t), y_tip], [s / 2 - ha_r * t, y_tip],
                    [s / 2 + hf * t, y_root], [-(s / 2 + hf * t), y_root]]);
        }
    }
}

module rack() {                     // 局所座標のまま（u, y, z は床基準）
    difference() {
        union() {
            translate([0, 0, rack_z0]) linear_extrude(rack_h) rack_2d();
            translate([leg_u0, y_tip, rack_z0])
                cube([leg_len, y_back - y_tip, base_t - rack_z0]);
        }
        for (u = screw_u)
            translate([u, screw_y, rack_z0 + 0.6]) cylinder(d = screw_pilot_d, h = base_t);
    }
}

// ---------------- キャリア ----------------
// 右キャリアの局所座標（u = x - J）。u = 0 が当たり面（パッド表面）。
module carrier() {
    z0 = base_t;
    jaw_u = pad_t;                  // 顎の実体面
    difference() {
        union() {
            // 板: 顎の範囲は顎面まで、それ以外は edge_recess だけ下げる
            translate([edge_recess, -car_half_y, z0])
                cube([car_len - edge_recess, 2 * car_half_y, car_t]);
            translate([jaw_u, -jaw_len / 2, z0])
                cube([car_len - jaw_u, jaw_len, car_t]);
            // 顎（上端内側に 45 度の導入面）
            translate([0, jaw_len / 2, z0]) rotate([90, 0, 0])
                linear_extrude(jaw_len) polygon([
                    [jaw_u, 0], [jaw_u + jaw_t, 0], [jaw_u + jaw_t, jaw_h],
                    [jaw_u + lead, jaw_h], [jaw_u, jaw_h - lead]]);
            // 輪ゴムフック
            for (sy = [-1, 1]) translate([hook_u, sy * hook_y, z0 + car_t]) {
                cylinder(d = hook_d, h = hook_h);
                translate([0, 0, hook_h]) cylinder(d = cap_d, h = cap_t);
            }
        }
        for (u = screw_u)
            translate([u, screw_y, z0 - 1]) cylinder(d = screw_hole_d, h = car_t + 2);
    }
}

module pad() {                      // 局所座標（u = 0〜pad_t）
    translate([0, -jaw_len / 2, base_t]) cube([pad_t, jaw_len, jaw_h - lead]);
}

// ---------------- ベース ----------------
// 右側（+Y）のトンネル。左側は 180 度回転。
module tunnel_2d() {
    translate([J_min + bar_lo - 0.5, y_tip - slide_clr])
        square([J_max + bar_hi + 0.5 - (J_min + bar_lo - 0.5), y_back - y_tip + 2 * slide_clr]);
}

module slot_2d() {                  // 脚が通る天井の貫通スロット（両端がストッパ）
    translate([J_min + leg_u0 - stop_clr, y_tip - slide_clr])
        square([travel + leg_len + 2 * stop_clr, y_back - y_tip + 2 * slide_clr]);
}

module recess_2d() {                // 床側から掘る範囲（天井 roof_t を残す）
    difference() {
        union() {
            circle(r = pocket_r, $fn = 96);
            for (a = [0, 180]) rotate(a) tunnel_2d();
        }
        circle(d = boss_d, $fn = 48);
    }
}

module mount_hole(pt) {             // 皿ねじ用の長穴（X 方向）
    hull() for (dx = [-mount_slot / 2, mount_slot / 2])
        translate([pt[0] + dx, pt[1], 0]) {
            translate([0, 0, -1]) cylinder(d = mount_d, h = base_t + 2);
            translate([0, 0, base_t - (csk_d - mount_d) / 2])
                cylinder(d1 = mount_d, d2 = csk_d, h = (csk_d - mount_d) / 2 + 0.01);
            translate([0, 0, base_t]) cylinder(d = csk_d, h = 1);
        }
}

module base() {
    difference() {
        linear_extrude(base_t)
            offset(r = base_r) offset(delta = -base_r)
                square([base_x, base_y], center = true);
        translate([0, 0, -1]) linear_extrude(base_t - roof_t + 1) recess_2d();
        translate([0, 0, -1]) linear_extrude(base_t + 2)
            for (a = [0, 180]) rotate(a) slot_2d();
        for (pt = mount_pts) mount_hole(pt);
        for (pt = plate_screw_pts)
            translate([pt[0], pt[1], -1]) cylinder(d = plate_pilot_d, h = plate_pilot_depth + 1);
    }
}

module base_outline_2d() {
    offset(r = base_r) offset(delta = -base_r) square([base_x, base_y], center = true);
}

// 裏板: ピニオン・ラックの抜け止め。床に置いたとき Z = -plate_t〜0。
module bottom_plate() {
    difference() {
        translate([0, 0, -plate_t]) linear_extrude(plate_t) base_outline_2d();
        for (pt = mount_pts)        // 床固定 M3 の長穴（ベースと同位置）
            hull() for (dx = [-mount_slot / 2, mount_slot / 2])
                translate([pt[0] + dx, pt[1], -plate_t - 1]) cylinder(d = mount_d, h = plate_t + 2);
        for (pt = plate_screw_pts)  // M2 皿ねじ（床側から、頭が面一以下）
            translate([pt[0], pt[1], -plate_t]) {
                translate([0, 0, -1]) cylinder(d = plate_hole_d, h = plate_t + 2);
                translate([0, 0, -0.01])
                    cylinder(d1 = plate_csk_d, d2 = plate_hole_d, h = (plate_csk_d - plate_hole_d) / 2);
            }
    }
}

// ---------------- 配置 ----------------
module placed_right(j) { translate([j, 0, 0]) children(); }
module placed_left(j) { rotate(180) translate([j, 0, 0]) children(); }

module assembly(w) {
    j = jaw_pos(w);
    color("lightgray") base();
    color("dimgray") bottom_plate();
    color("orange") rotate(pinion_angle(j)) pinion();
    color("steelblue") { placed_right(j) rack(); placed_left(j) rack(); }
    color("seagreen") { placed_right(j) carrier(); placed_left(j) carrier(); }
    color("black") { placed_right(j) pad(); placed_left(j) pad(); }
    if (w >= 2 * J_min)
        color("gold", 0.6) translate([-w / 2, -spec_L / 2, base_t]) cube([w, spec_L, spec_t]);
}

// 歯の干渉確認（2D）: ピニオンと左右ラックの共通部分。空なら OK。
module check2d(w) {
    j = jaw_pos(w);
    intersection() {
        rotate(pinion_angle(j)) pinion_2d();
        union() {
            translate([j, 0]) rack_2d();
            rotate(180) translate([j, 0]) rack_2d();
        }
    }
}

// 部品どうしの干渉確認（3D）: 空なら OK。
module check3d(w) {
    j = jaw_pos(w);
    intersection() {
        base();
        union() {
            rotate(pinion_angle(j)) pinion();
            placed_right(j) rack();
            placed_left(j) rack();
        }
    }
}

if (part == "base") rotate([180, 0, 0]) base();   // 支持面をベッド側にして印刷
else if (part == "plate") translate([0, 0, plate_t]) bottom_plate();
else if (part == "pinion") pinion();
else if (part == "rack") rack();
else if (part == "carrier") carrier();
else if (part == "pad") pad();
else if (part == "check2d") check2d(W);
else if (part == "check3d") check3d(W);
else assembly(W);
