import numpy as np
import cv2
import math

img = cv2.imread("bad_apple.png")
if img is None:
    print("Error: Could not read image.")
    exit()

#####################################
########### 每一幀都重新計算 ##########
#####################################
def transform(img, led_num, num_slices):

    side_len = min(img.shape[0:2])
    center = side_len // 2
    y_center = img.shape[0] // 2
    x_center = img.shape[1] // 2
    r_step = center / led_num
    # 裁切成正方形
    img2 = img[ (y_center - center):(y_center + center), 
                (x_center - center):(x_center + center)]

    # 宣告兩組燈條的資料陣列
    slices_A = [] # 第一條燈條 (正常)
    slices_B = [] # 第二條燈條 (向外偏移半格，且安裝在對面)

    for i in range(num_slices):
        sub_slice_A = []
        sub_slice_B = []
        
        # 算出目前的基礎角度 (轉為弧度)
        angle_A = i * 2 * np.pi / num_slices
        # Strip B 的物理位置在對面，所以取樣角度要加 180 度 (pi)
        angle_B = angle_A + np.pi

        for j in range(led_num):
            # ------------------------------------------------
            # 處理 Strip A (正常取樣)
            # ------------------------------------------------
            d_A = r_step * j
            y_A = int(center + d_A * np.cos(angle_A))
            x_A = int(center - d_A * np.sin(angle_A))
            
            # 邊界保護 (非常重要，避免 IndexError)
            if 0 <= x_A < side_len and 0 <= y_A < side_len:
                pixel_A = img[y_A, x_A]
            else:
                pixel_A = np.array([0, 0, 0], dtype=np.uint8) # 黑色
                
            sub_slice_A.append(pixel_A)

            # ------------------------------------------------
            # 處理 Strip B (交錯取樣)
            # ------------------------------------------------
            # 關鍵 1：半徑加上 r_step / 2.0 (向外推半格)
            d_B = r_step * j + (r_step / 2.0)
            # d_B = r_step * j
            
            # 關鍵 2：使用 angle_B (差 180 度的方向)
            y_B = int(center + d_B * np.cos(angle_B))
            x_B = int(center - d_B * np.sin(angle_B))
            
            if 0 <= x_B < side_len and 0 <= y_B < side_len:
                pixel_B = img[y_B, x_B]
            else:
                pixel_B = np.array([0, 0, 0], dtype=np.uint8)
                
            sub_slice_B.append(pixel_B)

        slices_A.append(sub_slice_A)
        slices_B.append(sub_slice_B)

    # ==========================================
    # 驗證與重建顯示
    # ==========================================
    output_size = side_len
    reconstructed = np.zeros((output_size, output_size, 3), dtype=np.uint8)

    # 計算畫圓點的大小 (稍微縮小一點以看出交錯效果)
    circle_radius = max(1, int(r_step // 2.5))

    for i in range(num_slices):
        angle_A = i * 2 * np.pi / num_slices
        angle_B = angle_A + np.pi
        
        for j in range(led_num):
            # 畫回 Strip A 的 LED 點
            d_A = r_step * j
            y_A = int(center + d_A * np.cos(angle_A))
            x_A = int(center - d_A * np.sin(angle_A))
            cv2.circle(reconstructed, (x_A, y_A), circle_radius, slices_A[i][j].tolist(), -1)

            # 畫回 Strip B 的 LED 點 (你會看到它完美卡在 Strip A 的空隙中)
            d_B = r_step * j + (r_step / 2.0)
            y_B = int(center + d_B * np.cos(angle_B))
            x_B = int(center - d_B * np.sin(angle_B))
            cv2.circle(reconstructed, (x_B, y_B), circle_radius, slices_B[i][j].tolist(), -1)
    return reconstructed

# vid = cv2.VideoCapture("bad_apple.mp4")

# while True:
#     ret, frame = vid.read()             # 讀取影片的每一幀
#     if not ret:
#         print("Cannot receive frame")   # 如果讀取錯誤，印出訊息
#         break

#     transformed = transform(frame, 20, 360)
    
#     cv2.imshow("Reconstructed Double Resolution", transformed)

#     if cv2.waitKey(1) == ord('q'):      # 每一毫秒更新一次，直到按下 q 結束
#         break


#####################################
########### 只計算一次，之後複用 ##########
#####################################
led_num = 20
num_slices = 360


# def calc_pixel(img, led_num, num_slices):

#     side_len = min(img.shape[0:2])
#     center = side_len // 2
#     y_center = img.shape[0] // 2
#     x_center = img.shape[1] // 2
#     r_step = center / led_num
#     # 裁切成正方形
#     img2 = img[ (y_center - center):(y_center + center), 
#                 (x_center - center):(x_center + center)]

#     # 宣告兩組燈條的資料陣列
#     slices_A = [] # 第一條燈條 (正常)
#     slices_B = [] # 第二條燈條 (向外偏移半格，且安裝在對面)

#     for i in range(num_slices):
#         sub_slice_A = []
#         sub_slice_B = []
        
#         # 算出目前的基礎角度 (轉為弧度)
#         angle_A = i * 2 * np.pi / num_slices
#         # Strip B 的物理位置在對面，所以取樣角度要加 180 度 (pi)
#         angle_B = angle_A + np.pi

#         for j in range(led_num):
#             # ------------------------------------------------
#             # 處理 Strip A (正常取樣)
#             # ------------------------------------------------
#             d_A = r_step * j
#             x_A = int(center - d_A * np.sin(angle_A))
#             y_A = int(center + d_A * np.cos(angle_A))
            
#             pixel_A = (x_A, y_A)
                
#             sub_slice_A.append(pixel_A)

#             # ------------------------------------------------
#             # 處理 Strip B (交錯取樣)
#             # ------------------------------------------------
#             # 關鍵 1：半徑加上 r_step / 2.0 (向外推半格)
#             d_B = r_step * j + (r_step / 2.0)
#             # d_B = r_step * j
            
#             # 關鍵 2：使用 angle_B (差 180 度的方向)
#             x_B = int(center - d_B * np.sin(angle_B))
#             y_B = int(center + d_B * np.cos(angle_B))
            
#             pixel_B = (x_B, y_B)
                
#             sub_slice_B.append(pixel_B)

#         slices_A.append(sub_slice_A)
#         slices_B.append(sub_slice_B)
#     return slices_A, slices_B, side_len, center, y_center, x_center

# vid = cv2.VideoCapture("bad_apple.mp4")
# ret, frame = vid.read()

# pix_a, pix_b, side_len, center, y_center, x_center = calc_pixel(frame, led_num, num_slices)
# r_step = side_len // 2 / led_num
# circle_radius = max(1, int(r_step // 2.5))

# while True:
#     ret, frame = vid.read()             # 讀取影片的每一幀
#     if not ret:
#         print("Cannot receive frame")   # 如果讀取錯誤，印出訊息
#         break
#     frame_cropped = frame[(y_center - center):(y_center + center), 
#                           (x_center - center):(x_center + center)]

#     reconstructed = np.zeros((side_len, side_len, 3), dtype=np.uint8)
#     for i in range(num_slices):
#         for j in range(led_num):
#             cv2.circle(reconstructed, pix_a[i][j], circle_radius, frame_cropped[pix_a[i][j][1]][pix_a[i][j][0]].tolist(), -1)
#             cv2.circle(reconstructed, pix_b[i][j], circle_radius, frame_cropped[pix_b[i][j][1]][pix_b[i][j][0]].tolist(), -1)
    
#     cv2.imshow("Reconstructed Double Resolution", reconstructed)

#     if cv2.waitKey(1) == ord('q'):      # 每一毫秒更新一次，直到按下 q 結束
#         break




#####################################
########### 只計算一次，之後複用 ##########
#####################################

def calc_pixel(img, led_num, num_slices):
    side_len = min(img.shape[0:2])
    center = side_len // 2
    r_step = center / led_num
    
    # 直接建立存放 X 和 Y 座標的 NumPy 陣列 (360, 40)
    lut_x_A = np.zeros((num_slices, led_num), dtype=np.int32)
    lut_y_A = np.zeros((num_slices, led_num), dtype=np.int32)
    lut_x_B = np.zeros((num_slices, led_num), dtype=np.int32)
    lut_y_B = np.zeros((num_slices, led_num), dtype=np.int32)

    for i in range(num_slices):
        angle_A = (i * 2 * np.pi / num_slices)
        angle_B = angle_A + np.pi

        for j in range(led_num):
            # Strip A
            d_A = r_step * j
            x_A = int(center - d_A * np.sin(angle_A))
            y_A = int(center + d_A * np.cos(angle_A))
            lut_x_A[i, j] = min(max(x_A, 0), side_len - 1)
            lut_y_A[i, j] = min(max(y_A, 0), side_len - 1)

            # Strip B
            d_B = r_step * j + (r_step / 2.0)
            x_B = int(center - d_B * np.sin(angle_B))
            y_B = int(center + d_B * np.cos(angle_B))
            lut_x_B[i, j] = min(max(x_B, 0), side_len - 1)
            lut_y_B[i, j] = min(max(y_B, 0), side_len - 1)

    return lut_x_A, lut_y_A, lut_x_B, lut_y_B, side_len, center

vid = cv2.VideoCapture("bad_apple.mp4")
ret, frame = vid.read()

lut_x_A, lut_y_A, lut_x_B, lut_y_B, side_len, center = calc_pixel(frame, led_num, num_slices)
y_center, x_center = frame.shape[0] // 2, frame.shape[1] // 2
r_step = side_len // 2 / led_num
circle_radius = max(1, int(r_step // 2.5))

while True:
    ret, frame = vid.read() 
    if not ret:
        break
        
    frame_cropped = frame[(y_center - center):(y_center + center), 
                          (x_center - center):(x_center + center)]

    # ⭐️ 核心黑魔法：瞬間抓取所有顏色！
    # 這行執行完，colors_A 直接變成形狀為 (360, 40, 3) 的 RGB 陣列
    colors_A = frame_cropped[lut_y_A, lut_x_A]
    colors_B = frame_cropped[lut_y_B, lut_x_B]

    # --- 以下是為了在電腦上「視覺化模擬」才需要的 for 迴圈 ---
    # 真正的硬體專題中，你只要把上面那包 colors_A 丟進 SPI 就完工了！
    reconstructed = np.zeros((side_len, side_len, 3), dtype=np.uint8)
    for i in range(num_slices):
        for j in range(led_num):
            # 取畫圖用的座標
            pt_a = (lut_x_A[i, j], lut_y_A[i, j])
            pt_b = (lut_x_B[i, j], lut_y_B[i, j])
            
            # 從剛剛瞬間抓好的顏色陣列中拿顏色來畫圖
            cv2.circle(reconstructed, pt_a, circle_radius, colors_A[i, j].tolist(), -1)
            cv2.circle(reconstructed, pt_b, circle_radius, colors_B[i, j].tolist(), -1)
            
    cv2.imshow("Reconstructed", reconstructed)
    if cv2.waitKey(1) == ord('q'): 
        break