import numpy as np
import cv2
import math

img = cv2.imread("bad_apple.png")
if img is None:
    print("Error: Could not read image.")
    exit()


led_num = 20
num_slices = 360



#####################################
########### 只計算一次，之後複用 ##########
#####################################

def calc_pixel(img, led_num, num_slices):
    side_len = min(img.shape[0:2])
    center = side_len // 2
    r_step = center / led_num
    
    # 直接建立存放 X 和 Y 座標的 NumPy 陣列 (360, 40)
    # lut_x_A = np.zeros((num_slices, led_num), dtype=np.int32)
    # lut_y_A = np.zeros((num_slices, led_num), dtype=np.int32)
    # lut_x_B = np.zeros((num_slices, led_num), dtype=np.int32)
    # lut_y_B = np.zeros((num_slices, led_num), dtype=np.int32)

    lut_x = np.zeros((num_slices, led_num*2), dtype=np.int32)
    lut_y = np.zeros((num_slices, led_num*2), dtype=np.int32)

    for i in range(num_slices):
        angle_A = (i * 2 * np.pi / num_slices)
        angle_B = angle_A + np.pi

        for j in range(led_num):
            # Strip A
            d_A = r_step * j
            x_A = int(center - d_A * np.sin(angle_A))
            y_A = int(center + d_A * np.cos(angle_A))
            # lut_x_A[i, j] = min(max(x_A, 0), side_len - 1)
            # lut_y_A[i, j] = min(max(y_A, 0), side_len - 1)

            # Strip B
            d_B = r_step * j + (r_step / 2.0)
            x_B = int(center - d_B * np.sin(angle_B))
            y_B = int(center + d_B * np.cos(angle_B))
            # lut_x_B[i, j] = min(max(x_B, 0), side_len - 1)
            # lut_y_B[i, j] = min(max(y_B, 0), side_len - 1)

            lut_x[i, j] = min(max(x_B, 0), side_len - 1)
            lut_x[i, j+led_num] = min(max(x_A, 0), side_len - 1)

            lut_y[i, j] = min(max(y_B, 0), side_len - 1)
            lut_y[i, j+led_num] = min(max(y_A, 0), side_len - 1)

    # return lut_x_A, lut_y_A, lut_x_B, lut_y_B, side_len, center
    return lut_x, lut_y, side_len, center

vid = cv2.VideoCapture("test.mp4")
ret, frame = vid.read()

lut_x, lut_y, side_len, center = calc_pixel(frame, led_num, num_slices)
y_center, x_center = frame.shape[0] // 2, frame.shape[1] // 2
r_step = side_len // 2 / led_num
# circle_radius = max(1, int(r_step // 2.5))
circle_radius = 3

while True:
    ret, frame = vid.read() 
    if not ret:
        break
        
    frame_cropped = frame[(y_center - center):(y_center + center), 
                          (x_center - center):(x_center + center)]

    # ⭐️ 核心黑魔法：瞬間抓取所有顏色！
    # 這行執行完，colors_A 直接變成形狀為 (360, 40, 3) 的 RGB 陣列
    colors = frame_cropped[lut_y, lut_x]

    # --- 以下是為了在電腦上「視覺化模擬」才需要的 for 迴圈 ---
    # 真正的硬體專題中，你只要把上面那包 colors_A 丟進 SPI 就完工了！
    reconstructed = np.zeros((side_len, side_len, 3), dtype=np.uint8)
    for i in range(num_slices):
        for j in range(led_num):
            # 取畫圖用的座標


            pt_a = (lut_x[i, j+led_num], lut_y[i, j+led_num])
            pt_b = (lut_x[i, j], lut_y[i, j])
            
            # 從剛剛瞬間抓好的顏色陣列中拿顏色來畫圖
            cv2.circle(reconstructed, pt_a, circle_radius, colors[i, j+led_num].tolist(), -1)
            cv2.circle(reconstructed, pt_b, circle_radius, colors[i, j].tolist(), -1)
            
    cv2.imshow("Reconstructed", reconstructed)
    if cv2.waitKey(1) == ord('q'): 
        break