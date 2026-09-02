# model/

`yolov8.rknn` 是用于开箱即跑的 **演示模型**（YOLOv8n / COCO 80 类，640×640 输入），
从 [rknn_model_zoo](https://github.com/airockchip/rknn_model_zoo) 的 YOLOv8 示例
经 RKNN-Toolkit2 转换得到。

- 目标平台：RK3588（RV1106/1103 编译宏未启用）
- 输入：RGB888，NHWC，640×640
- 输出：3 个检测头，量化类型 AFFINE_ASYMMETRIC

## 替换成自己的模型

1. 用 PyTorch 训练 YOLOv8，导出 ONNX：
   `yolo export model=best.pt format=onnx opset=12`
2. 用 [RKNN-Toolkit2](https://github.com/airockchip/rknn-toolkit2) 转成 `.rknn`：

   ```python
   from rknn.api import RKNN
   rknn = RKNN()
   rknn.config(target_platform="rk3588")
   rknn.load_onnx(model="best.onnx")
   rknn.build(do_quantization=True, dataset="dataset.txt")
   rknn.export_rknn("yolov8.rknn")
   ```

3. 运行：`./v4l2_yolov_dual_mpp ./model/yolov8.rknn ...`

> 类别标签目前由 `support/postprocess.cc` 内置的 COCO-80 表给出
> （`coco_cls_to_name()`）；换成自有模型时请同步修改标签表。

---

`yolov8.rknn` is a **demo model** (YOLOv8n / COCO-80, 640×640 input) so the
pipeline runs out of the box. It is converted from the
[rknn_model_zoo](https://github.com/airockchip/rknn_model_zoo) YOLOv8 example
with RKNN-Toolkit2, target `rk3588`.

To use your own model, export ONNX from PyTorch and convert with RKNN-Toolkit2
as shown above. Note the class labels are hard-coded to COCO-80 in
`support/postprocess.cc` (`coco_cls_to_name()`); update them when you swap models.
