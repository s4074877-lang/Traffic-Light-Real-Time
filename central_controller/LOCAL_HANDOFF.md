# Tin nhắn bàn giao sau bản Local mới

Các đoạn dưới đây là bản nháp để gửi nhóm; chưa được gửi. Các kết luận về Local
dựa trên đọc source, chưa phải kết quả chạy tích hợp QNX bản mới.

## 1. Phần Central và contract simulation

> T đối chiếu bản Local mới rồi. Chốt theo chat: simulation giữ ở Local, Central gửi request. T đã bổ sung phía Central `sim-start I1`, `sim-stop I1`, `sim-time I1 07:00`, kiểm tra lệnh/ID và hiển thị WALK/STOP. M cần nối handler Local theo mục “Local traffic-simulation command contract” trong `central_controller/INTEGRATION.md`: dùng `MSG_TEST` với chuỗi `SIM1`, trả đúng `command_id`. Handler hiện tại chỉ ghi timestamp rồi ACK ID 0 nên chưa điều khiển simulation được. Stop chỉ dừng sinh xe, bộ đèn/pedestrian/railway vẫn chạy. Tạm demo I1 và không bật `--schedule` của Central để dùng giờ mô phỏng Local. Ghép xong mình chạy QNX kiểm tra, chưa chốt đã pass end to end nhé.

## 2. Trả lời WALK và Yellow

> WALK là trạng thái cho người đi bộ đi, có thể biểu diễn bằng đèn xanh pedestrian. Nhưng cần chốt nhãn pedestrian NS/EW trên sơ đồ là hướng người đi hay đường băng qua, rồi mới đối chiếu với đèn xe. Hiện code cho ped_NS WALK lúc EW Green; request ped_NS cũng đang cắt EW Green, khác mô tả “cắt Green cùng hướng còn 10 giây”. Mình chốt mapping rồi sửa điều kiện cho thống nhất nhé. Vẫn giữ Yellow: đỏ -> xanh trực tiếp, xanh -> vàng -> đỏ. Chat trước chưa xác nhận bỏ vàng.

## 3. Các điểm Local cần sửa trước khi duyệt

> T thấy thêm mấy chỗ cần sửa bên Local: revert khi không có temp đang bỏ luôn manual override; temp chồng temp làm mất baseline ban đầu; coordination ACK nhưng chưa dùng offset. Fail-safe cần latch all-red, vì hiện check sau khi ghi lại đèn và HOLD có đường về Green ở tick sau. Nếu train thật đến lúc train giả đang pending/active thì phải chuyển sang chờ `MSG_TRAIN_CLEAR`, không bỏ qua message rồi tự clear theo countdown cũ. Chi tiết có trong `INTEGRATION.md`; sửa xong mình test mode/temp khi Central offline, pedestrian và train/fail-safe rồi update implementation note theo kết quả thực tế.
