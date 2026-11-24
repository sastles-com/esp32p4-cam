// 70x150 プレート撮影用固定治具 v2（3点固定・角バネ版）
// 仕様: docs/instructions/plate_70x150_jig_spec_v2.md
//
// プレートは左上（-X,+Y）の基準角の当たり3点に，右下（+X,-Y）の角バネで押し当てる．
//   当たり: +Y 辺に 2 点（x = ±datum_span/2），-X 辺に 1 点（y = datum_x_y）
//   角バネ: X 腕が右辺を -X へ，Y 腕が下辺を +Y へ押す（別部品，PETG）
// 原点 = 当たりに押し当てた状態のプレート中心．X = 長辺，Y = 短辺，z = 0 はベース下面．
// OpenSCAD 2021.01 で動く構文だけを使う．
//
// 仮定値（未確認）: plate_t，プレート重さ（鋼板 1mm），摩擦係数 0.3，床寸法 174x100．README 参照．

/* [出力] */
part = "assembly";          // [assembly, base, spring]
show_plate = true;          // assembly でプレートを半透明で表示

/* [プレート（plate_t は仮定値）] */
plate_l = 150;
plate_w = 70;
plate_t = 1.0;

/* [ベース・ガイド] */
base_l = 174;
base_w = 100;
base_t = 4;
guide_clearance = 1.0;      // プレート縁とガイド壁のすきま（片側）
guide_h = 3;                // ガイド壁の高さ（ベース上面から）
wall_t = 3;

/* [当たり] */
datum_span = 100;           // +Y 辺の 2 点の間隔
datum_r = 2;
datum_x_y = 28;             // -X 辺の 1 点の y（基準角寄り）

/* [角バネ（PETG）] */
spring_l = 30;              // 腕の長さ
spring_t = 1.6;             // 腕の厚さ
spring_above = 3;           // 角バネ上面のプレート上面からの高さ
spring_preload = 2.8;       // プレートを置いた状態のたわみ（最初の版と同じ押し付け力 0.85N）
spring_overtravel = 0.8;    // 入れるときの追加たわみ（返しの張り出し hook_lip*tan(hook_angle) より十分大きく）
spring_E = 2000;            // PETG のヤング率 [MPa]
pad_w = 5;                  // パッドの幅（辺に沿う長さ）
hook_angle = 20;            // 返し（逆テーパー）の角度 [deg]．パッド 2 か所と当たり 3 点に共通．
                            // 当たり面は上ほどプレート側へ出て，プレートの上の縁を押さえ込む
pad_relief = 0.4;           // 当たり面の下端の逃げ（45° 面取り）．一層目のはみ出しがプレート縁の下に入らないように
hook_lip = 0.8;             // 返しの上端のプレート上面からの高さ．その上は鉛直
pad_top_chamfer = 0.4;      // パッド上端の角の面取り（斜面は付けない）
pad_style = "hook";         // [hook, ramp] hook = 返し付き（現行）．ramp = 最初の版（鉛直の当たり面＋上に差し込み用の斜面）
fillet_r = 3;
fit = 0.15;                 // 取付ブロックと凹みのすきま

/* [爪（オプション）] */
claw_enable = false;
claw_overlap = 3;
claw_gap = 0.5;
claw_thick = 1.2;
claw_len = 10;

/* [固定ねじ穴（仮仕様）] */
hole_enable = true;
hole_d = 3.2;
hole_inset = 8;

/* [Hidden] */
$fn = 48;
eps = 0.01;

// ---- 導出値 --------------------------------------------------------------
px = plate_l / 2;  py = plate_w / 2;               // プレートの半寸法
qx = px + guide_clearance;  qy = py + guide_clearance;   // ポケットの半寸法
bx = base_l / 2;  by = base_w / 2;
z_floor = base_t - plate_t;                        // ポケット底
z_wall = base_t + guide_h;                         // ガイド壁の上面
spring_h = plate_t + spring_above;

// 角バネの配置（自由状態，プレートが無いときの形）
blk = [[qx + 1, -by + 1], [bx - 1, -qy - 1]];      // 取付ブロック [[x0,y0],[x1,y1]]
xa = qx + 4.5;                                     // X 腕の中心 x
ya = -(qy + 4.5);                                  // Y 腕の中心 y
xarm_y1 = blk[1][1] + spring_l;                    // X 腕の先端 y
yarm_x0 = blk[0][0] - spring_l;                    // Y 腕の先端 x
xpad_face = px - spring_preload;                   // 自由状態の X パッドの当たり面
ypad_face = -py + spring_preload;                  // 自由状態の Y パッドの当たり面
ch_z = plate_t + hook_lip;                         // 返しの上端（パッド基準の高さ）
hook_over = hook_lip * tan(hook_angle);            // 返しのプレート上への張り出し
travel = spring_preload + spring_overtravel + 1;   // 腕の溝の逃げ
screw_xy = [[blk[1][0] - 3.5, blk[0][1] + 3], [blk[1][0] - 3.5, blk[1][1] - 4]];

// ---- 計算値（README と一致させる）--------------------------------------------
I = spring_h * pow(spring_t, 3) / 12;
k = 3 * spring_E * I / pow(spring_l, 3);
F = k * spring_preload;
sig = 3 * spring_E * spring_t * spring_preload / (2 * spring_l * spring_l);
sig_max = 3 * spring_E * spring_t * (spring_preload + spring_overtravel) / (2 * spring_l * spring_l);
ys = xarm_y1 - pad_w / 2;                          // X 腕が押す位置 y
xs = yarm_x0 + pad_w / 2;                          // Y 腕が押す位置 x
dr = -(ys + xs - datum_x_y) * F / (datum_span / 2);  // 長辺側 2 点の力の差（左 - 右）．モーメントのつり合いから
echo(str("spring: k=", k, " N/mm  F=", F, " N/arm  sigma=", sig, " MPa  sigma_max=", sig_max, " MPa"));
echo(str("datum forces: -X pad=", F, " N  +Y pads left=", (F + dr) / 2, " N right=", (F - dr) / 2, " N"));
echo(str("push points: X arm y=", ys, "  Y arm x=", xs));
echo(str("hook: angle=", hook_angle, " deg  overhang=", hook_over, " mm  hold-down per spring pad=", F * tan(hook_angle),
         " N  contact band z=", pad_relief, "..", plate_t));
if (sig_max > 20) echo("WARNING: sigma_max > 20 MPa");
if (spring_overtravel < hook_over + 0.5) echo("WARNING: overtravel too small to clear the datum hooks");
if (pad_relief >= plate_t - 0.2) echo("WARNING: pad_relief leaves too little contact band");
if ((F - dr) / 2 <= 0 || (F + dr) / 2 <= 0) echo("WARNING: a datum pad gets no force");
if (xa + spring_t / 2 + travel > bx - 1) echo("WARNING: base_l too small for the X arm travel");
if (ya - spring_t / 2 - travel < -by + 1) echo("WARNING: base_w too small for the Y arm travel");

// ---- 部品 ----------------------------------------------------------------

module box(x0, y0, z0, x1, y1, z1) {
    translate([x0, y0, z0]) cube([x1 - x0, y1 - y0, z1 - z0]);
}

// 内側上端に 1mm の面取りがある壁．inner = 内側の面の座標，dir = 外側の向き（+1/-1）
module wall_x(x0, x1, inner, dir) {     // X 方向に伸びる壁（内面は y = inner）
    y_out = inner + dir * wall_t;
    hull() {
        box(x0, min(inner, y_out), base_t - eps, x1, max(inner, y_out), z_wall - 1);
        box(x0, min(inner + dir, y_out), base_t - eps, x1, max(inner + dir, y_out), z_wall);
    }
}
module wall_y(y0, y1, inner, dir) {     // Y 方向に伸びる壁（内面は x = inner）
    x_out = inner + dir * wall_t;
    hull() {
        box(min(inner, x_out), y0, base_t - eps, max(inner, x_out), y1, z_wall - 1);
        box(min(inner + dir, x_out), y0, base_t - eps, max(inner + dir, x_out), y1, z_wall);
    }
}

function in_rect(p, r, m) = p[0] > r[0][0] - m && p[0] < r[1][0] + m && p[1] > r[0][1] - m && p[1] < r[1][1] + m;

// 取付凹みと腕の溝（ポケットと同じ深さ）
recess = [[blk[0][0] - fit, blk[0][1] - fit], [blk[1][0] + fit, blk[1][1] + fit]];
xchan = [[qx - eps, blk[1][1] - eps], [xa + spring_t / 2 + travel, xarm_y1 + 2]];
ychan = [[yarm_x0 - 2, ya - spring_t / 2 - travel], [blk[0][0] + eps, -qy + eps]];
keepout = [recess, xchan, ychan, [[-qx - wall_t, -qy - wall_t], [qx, qy + wall_t]]];

module base() {
    difference() {
        union() {
            box(-bx, -by, 0, bx, by, base_t);
            wall_y(-qy - wall_t, qy + wall_t, -qx, -1);               // -X 壁
            wall_x(-qx - wall_t, qx + wall_t, qy, 1);                 // +Y 壁
            wall_x(-qx - wall_t, ychan[0][0] - 1, -qy, -1);           // -Y 壁（Y 腕の溝の手前まで）
        }
        // ポケット
        box(-qx, -qy, z_floor, qx, qy, z_wall + 1);
        // ポケットの角逃げ
        for (sx = [-1, 1], sy = [-1, 1])
            translate([sx * qx, sy * qy, z_floor]) cylinder(d = 2, h = z_wall);
        // 取付凹みと腕の溝
        for (r = [recess, xchan, ychan])
            box(r[0][0], r[0][1], z_floor, r[1][0], r[1][1], z_wall + 1);
        // 角バネの下穴（M3 セルフタップ）
        for (p = screw_xy) translate([p[0], p[1], -1]) cylinder(d = 2.6, h = base_t + 2);
        // 指掛け（+X 端，角バネより上）
        translate([qx + 5, py * 0.55, max(1, z_floor - 1.2)]) cylinder(r = 8, h = base_t);
        // 固定ねじ穴（かかるものは出さない）
        if (hole_enable)
            for (sx = [-1, 1], sy = [-1, 1]) {
                p = [sx * (bx - hole_inset), sy * (by - hole_inset)];
                if (!in_rect(p, keepout[0], hole_d / 2 + 0.5) && !in_rect(p, keepout[1], hole_d / 2 + 0.5)
                    && !in_rect(p, keepout[2], hole_d / 2 + 0.5) && !in_rect(p, keepout[3], hole_d / 2 + 0.5))
                    translate([p[0], p[1], -1]) cylinder(d = hole_d, h = base_t + 2);
            }
        // 刻線（原点を通る X・Y の線，ポケットの外側）
        for (x = [[-bx + 2, -qx - wall_t - 1.5], [xchan[1][0] + 1, bx - 2]])
            box(x[0], -0.3, base_t - 0.4, x[1], 0.3, base_t + 1);
        for (y = [[qy + wall_t + 1.5, by - 2], [-by + 2, -qy - wall_t - 1.5]])
            box(-0.3, y[0], base_t - 0.4, 0.3, y[1], base_t + 1);
        // 基準角（左上）の目印: 三角の刻印
        translate([0, 0, base_t - 0.5])
            linear_extrude(1)
                polygon([[-bx + 2, by - 2], [-bx + 7, by - 2], [-bx + 2, by - 7]]);
    }
    // 当たり（ポケット底からガイド壁の上面まで）
    translate([-(qx + datum_r - guide_clearance), datum_x_y, 0]) datum_pin();
    for (sx = [-1, 1])
        translate([sx * datum_span / 2, qy + datum_r - guide_clearance, 0]) datum_pin();
    // 爪（オプション，-X 側の 2 隅のみ）
    if (claw_enable)
        for (sy = [-1, 1])
            translate([-qx - eps, sy > 0 ? qy - 4 - claw_len : -qy + 4, base_t + claw_gap])
                cube([guide_clearance + claw_overlap, claw_len, claw_thick]);
}

// 当たり．返し付き（逆円錐）．プレート上面の高さで半径 datum_r（ここがプレートの上の縁に当たる），
// 下ほど細く，上はプレート上へ hook_over 張り出してから鉛直．上端に 0.4 の面取り．
module datum_pin() {
    r_top = datum_r + hook_over;
    rotate_extrude() polygon([[0, z_floor], [datum_r - plate_t * tan(hook_angle), z_floor],
                              [r_top, base_t + hook_lip], [r_top, z_wall - 0.4], [r_top - 0.4, z_wall], [0, z_wall]]);
}

// 片持ち腕の根元のフィレット（2D）．c = 角の点，dx,dy = フィレットが広がる向き
module fillet2d(c, dx, dy) {
    translate(c) difference() {
        scale([dx, dy]) square(fillet_r);
        translate([dx * fillet_r, dy * fillet_r]) circle(r = fillet_r);
    }
}

// パッドの断面．u = 当たり面からプレートと反対向きの距離（プレート上面の高さで u = 0），z = 高さ．
//   下端: 45° の逃げ（一層目のはみ出しがプレート縁の下に入り込んで持ち上げるのを防ぐ）
//   当たり面: 上ほどプレート側へ出る逆テーパー（押す力に下向きの成分）
//   上部: 鉛直（斜面は付けない．縁が斜面に乗ると押す力が上を向き，プレートが浮いて外れるため）
function pad_u(z) = -(z - plate_t) * tan(hook_angle);
module pad_profile(back) {
    if (pad_style == "ramp")   // 最初の版: 当たり面は鉛直でプレート上面＋0.3 まで，その上は斜面（水平幅 preload＋overtravel＋0.5）
        polygon([[back, 0], [0, 0], [0, plate_t + 0.3],
                 [spring_preload + spring_overtravel + 0.5, spring_h], [back, spring_h]]);
    else
    polygon([[back, 0], [pad_u(pad_relief) + pad_relief, 0], [pad_u(pad_relief), pad_relief],
             [pad_u(ch_z), ch_z], [pad_u(ch_z), spring_h - pad_top_chamfer],
             [pad_u(ch_z) + pad_top_chamfer, spring_h], [back, spring_h]]);
}

// 角バネ（自由状態の形，z = 0 が下面）
module spring() {
    difference() {
        union() {
            linear_extrude(spring_h) {
                translate(blk[0]) square([blk[1][0] - blk[0][0], blk[1][1] - blk[0][1]]);
                // X 腕（+Y へ）
                translate([xa - spring_t / 2, blk[1][1] - eps]) square([spring_t, spring_l + eps]);
                // Y 腕（-X へ）
                translate([yarm_x0, ya - spring_t / 2]) square([spring_l + eps, spring_t]);
                // 根元のフィレット
                fillet2d([xa - spring_t / 2, blk[1][1]], -1, 1);
                fillet2d([xa + spring_t / 2, blk[1][1]], 1, 1);
                fillet2d([blk[0][0], ya + spring_t / 2], -1, 1);
                fillet2d([blk[0][0], ya - spring_t / 2], -1, -1);
            }
            // X パッド: 右辺を -X へ押す．断面 (u, z) を (x = xpad_face + u, y = xarm_y1 - e) に置く
            multmatrix([[1, 0, 0, xpad_face], [0, 0, -1, xarm_y1], [0, 1, 0, 0]])
                linear_extrude(pad_w) pad_profile(xa - xpad_face);
            // Y パッド: 下辺を +Y へ押す．(x = yarm_x0 + e, y = ypad_face - u)
            multmatrix([[0, 0, 1, yarm_x0], [-1, 0, 0, ypad_face], [0, 1, 0, 0]])
                linear_extrude(pad_w) pad_profile(ypad_face - ya);
        }
        for (p = screw_xy) translate([p[0], p[1], -1]) cylinder(d = 3.3, h = spring_h + 2);
    }
}

module plate_ref() {
    color([0.3, 0.5, 0.9, 0.35]) box(-px, -py, z_floor, px, py, base_t);
}

// ---- 出力 ----------------------------------------------------------------
if (part == "base") base();
else if (part == "spring") spring();
else {
    color("Gainsboro") base();
    color("OrangeRed") translate([0, 0, z_floor]) spring();
    if (show_plate) %plate_ref();
}
