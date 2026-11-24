// 噛み合わせバーの共通寸法（rack_bar / bar_socket で共有）

/* [本体] */
L = 150;                 // 全長
W = 20;                  // 幅
stack_h = 20;            // 2本噛み合わせ時の合計厚さ

/* [歯] */
pitch = 5;               // 山の間隔
apex_angle = 45;         // 山の頂角
end_flat = 10;           // 両端の歯なし区間（谷底高さで平坦）
tip_r = 0.8;             // 山のR
root_r = 0.5;            // 谷のR（tip_r より小さくすること）
clr = 0.2;               // 噛み合いクリアランス（歯面の法線方向、2本合計）

half = apex_angle / 2;
tooth_h = (pitch / 2) / tan(half);          // 尖った歯の理論高さ
c = clr / 2;                                // 1本あたりの歯面オフセット
// 歯面オフセットで沈み込む分を補正して合計厚さを stack_h に保つ
base_t = (stack_h - tooth_h + 2 * c / sin(half)) / 2;

/* [スナップ（bar_socket の爪と，バー端の溝）] */
// バー端からの距離 x で測る．溝はバーの細い側面（幅方向の両端面）に，厚さ方向へ横断して入れる．
snap_fit = 0.2;          // ソケット内面とバーのすき間（bar_socket の fit と同じ）
snap_bead_d = 0.6;       // 爪の返しの張り出し（たわみ = snap_bead_d - snap_fit = 0.4）
snap_bead_x0 = 8.6;      // 返しの水平面（抜け止め面）の位置（バー端から）
snap_bead_h = 1.2;       // 返しの高さ（水平面から 45° 導入面の上端まで）
snap_groove_x0 = 8.4;    // 溝の範囲（バー端から）．抜け止め面との遊び 0.2
snap_groove_x1 = 10.0;   // 歯の始まり（end_flat）まで
snap_groove_d = 0.8;     // 溝の深さ
