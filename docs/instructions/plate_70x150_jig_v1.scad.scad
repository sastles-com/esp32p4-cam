// 70x150 プレート固定治具 v1 (パラメトリック)
// XD4撮影実験用．プレートを浅いポケットに置き，幅方向は両側壁でセンタリング，
// 長辺方向は片端の基準壁に板バネで押し付けて位置決めする．
// 原点はベース中心．X = 長辺(150mm)方向，Y = 短辺(70mm)方向．Z = 上向き．
// 座標: ベース下面が z=0，ベース上面(ポケット床)が z=base_t，
//       プレート上面と壁上面は z=base_t+plate_t で面一．
// 印刷: 底面をベッドに置いてそのまま印刷．サポート不要(爪の張り出しは3mm)．
// 単位はすべて mm．
//
// 要確認の仮定値:
//   plate_t = 1.0 ... プレート厚さ．チッピング試験片を 0.8〜1mm 級と仮定．
//                    実測値で置き換えること．ポケット深さと爪の高さが連動する．
//   base_l/base_w = 174x100 ... フレーム内側の床に収まることを未確認．
//                    収まらなければ base_l/base_w だけ縮める(ポケットは不変)．
//                    最小は壁と縁代を入れて約 169x89．

/* [プレート] */
plate_l = 150;      // プレート長辺
plate_w = 70;       // プレート短辺
plate_t = 1.0;      // プレート厚さ(要確認・仮定値)

/* [ポケット] */
clearance = 0.5;    // 片側のすきま
pocket_l = plate_l + 2 * clearance;   // 151
pocket_w = plate_w + 2 * clearance;   // 71

/* [ベース] */
base_l = 174;       // 要確認: フレーム内側の床寸法
base_w = 100;
base_t = 4;

/* [壁・爪・バネ] */
wall_t = 3;         // ポケット周囲の壁厚
wall_h = plate_t;   // 壁の高さ = プレート厚さ(プレート上面と壁上面が面一)
claw_overlap = 3;   // 爪のプレートへのかかり代
claw_len = 12;      // 爪の壁沿い長さ
claw_gap = 0.5;     // 爪下面とプレート上面のすきま(浮き上がり止め，締め付けない)
claw_thick = 1.5;   // 爪の厚さ
anchor_len = 12;    // +X端アンカーブロックのY方向長さ(爪の受け)
spring_t = 2.0;     // 板バネ(弓形)の板厚．力の調整はここか bow_depth で
bow_depth = 2.5;    // 弓の張り出し．静止時の接触面はポケット端から 2.5mm 内側
                    // プレート端は +75.0 なので，挿入時のたわみは約 2.0mm
scoop_r = 10;       // 指掛けスクープの半径
scoop_depth = 2.0;  // 指掛けスクープの深さ

/* [固定ねじ穴] */
hole_d = 3.2;       // M3用
hole_inset = 8;     // ベース縁からの穴中心距離

$fn = 64;

module base_plate() {
    difference() {
        // ベース本体(z 0〜base_t)
        translate([-base_l / 2, -base_w / 2, 0])
            cube([base_l, base_w, base_t]);
        // 固定ねじ穴 4か所(貫通)
        for (sx = [-1, 1], sy = [-1, 1])
            translate([sx * (base_l / 2 - hole_inset),
                       sy * (base_w / 2 - hole_inset), base_t / 2])
                cylinder(d = hole_d, h = base_t + 2, center = true);
        // 指掛けスクープ(+X端のポケット縁，半円のくぼみ)
        translate([pocket_l / 2, 0, base_t - scoop_depth])
            cylinder(r = scoop_r, h = scoop_depth + 0.1);
    }
}

// ポケット周囲の壁．-X端が基準壁，±Yが幅方向の壁．+X端は壁なし(板バネ側)．
// 壁はすべてベース上面(z=base_t)から wall_h だけ立ち上がる．
module rim_walls() {
    // -X 基準壁: x -78.5〜-75.5，y -38.5〜+38.5
    translate([-pocket_l / 2 - wall_t, -pocket_w / 2 - wall_t, base_t])
        cube([wall_t, pocket_w + 2 * wall_t, wall_h]);
    // ±Y 側壁: x -78.5〜+78.5，y ±(35.5〜38.5)
    for (sy = [-1, 1])
        translate([-pocket_l / 2 - wall_t,
                   sy > 0 ? pocket_w / 2 : -pocket_w / 2 - wall_t, base_t])
            cube([pocket_l + 2 * wall_t, wall_t, wall_h]);
    // +X端のアンカーブロック2個(板バネの両端と +X 側の爪を受ける)
    // x 75.5〜78.5，y ±(26.5〜38.5)
    for (sy = [-1, 1])
        translate([pocket_l / 2,
                   sy > 0 ? pocket_w / 2 + wall_t - anchor_len
                          : -pocket_w / 2 - wall_t, base_t])
            cube([wall_t, anchor_len, wall_h]);
}

// 弓形の板バネ．+X端でプレートを -X の基準壁へ押し付ける．
// 両端は ±Y 側壁の +X 端に埋まり，中央が -X 側へ bow_depth 張り出す．
// 両端にはフットを付け，側壁とアンカーブロックに確実に癒合させる．
module bow_spring() {
    chord = pocket_w + 2 * wall_t;       // 77(側壁の外端まで)
    R = ((chord / 2) * (chord / 2) + bow_depth * bow_depth) / (2 * bow_depth);
    cx = pocket_l / 2 - bow_depth + R;   // 円弧の中心 x
    a0 = asin((chord / 2) / R);
    n = 32;
    pts_outer = [for (i = [0:n])
        let (a = -a0 + 2 * a0 * i / n) [cx - R * cos(a), R * sin(a)]];
    pts_inner = [for (i = [n:-1:0])
        let (a = -a0 + 2 * a0 * i / n)
        [cx - (R - spring_t) * cos(a), (R - spring_t) * sin(a)]];
    union() {
        translate([0, 0, base_t])
            linear_extrude(height = wall_h)
                polygon(concat(pts_outer, pts_inner));
        // バネ端のフット: 弓の端・側壁・アンカーブロックを結ぶ
        for (sy = [-1, 1])
            translate([pocket_l / 2 - 2,
                       sy > 0 ? pocket_w / 2 : -pocket_w / 2 - 2 * wall_t,
                       base_t])
                cube([6, 2 * wall_t, wall_h]);
    }
}

// 四隅の爪．壁の上に載り，プレートの角へ 3mm かかる．締め付けず浮き上がりだけ止める．
// L字断面: 壁の上の部分は壁上面から爪上面までเต็มの高さで立ち，
// プレート上に張り出す部分だけ下面に 0.5mm のすきまを空ける．
module claws() {
    z_wall = base_t + plate_t;           // 壁上面 = プレート上面
    z_lip = z_wall + claw_gap;           // 張り出し部の下面
    for (sx = [-1, 1], sy = [-1, 1]) {
        y0 = sy > 0 ? pocket_w / 2 + wall_t - claw_len : -pocket_w / 2 - wall_t;
        // 壁上の脚部(壁上面から爪上面まで)．-X側は基準壁の上，+X側はアンカーの上．
        translate([sx > 0 ? pocket_l / 2 : -pocket_l / 2 - wall_t,
                   y0, z_wall])
            cube([wall_t, claw_len, claw_thick + claw_gap]);
        // プレート上への張り出し部(下面にすきまあり)
        translate([sx > 0 ? pocket_l / 2 - claw_overlap : -pocket_l / 2,
                   y0, z_lip])
            cube([claw_overlap, claw_len, claw_thick]);
    }
}

// 基準角の目印．-X壁の +Y 側上面に ø2 深さ1mmのくぼみ．
// (壁厚3mmに対し ø3 では両側面に接してしまうため ø2 とする)
module datum_mark() {
    translate([-pocket_l / 2 - wall_t / 2, pocket_w / 2 + wall_t / 2 - 4,
               base_t + wall_h - 1])
        cylinder(d = 2, h = 1.1);
}

// 参考: プレートの占有範囲(印刷しない．位置確認用に % で表示)
// translate([0, 0, base_t]) %cube([plate_l, plate_w, plate_t], center = true);

difference() {
    union() {
        base_plate();
        rim_walls();
        bow_spring();
        claws();
    }
    datum_mark();
}
