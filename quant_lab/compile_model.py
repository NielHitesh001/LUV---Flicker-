import treelite
import tl2cgen

model = treelite.frontend.load_xgboost_model("quant_lab/model.json")

tl2cgen.export_lib(
    model,
    toolchain="gcc",
    libpath="quant_lab/luv_model_raw.so",
    params={
        "parallel_comp": 4,
    },
)
