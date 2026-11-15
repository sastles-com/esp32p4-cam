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
