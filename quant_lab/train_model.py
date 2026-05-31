import xgboost as xgb
import pandas as pd
from sklearn.model_selection import train_test_split
from sklearn.metrics import accuracy_score, roc_auc_score

def main():
    df = pd.read_parquet("quant_lab/features.parquet")
    print(f"Loaded {len(df)} rows from features.parquet")

    X = df.drop(columns=["label"])
    y = df["label"]

    X_train, X_val, y_train, y_val = train_test_split(X, y, test_size=0.2, random_state=42)

    dtrain = xgb.DMatrix(X_train, label=y_train)
    dval = xgb.DMatrix(X_val, label=y_val)

    params = {
        "objective":        "binary:logistic",
        "eval_metric":      "auc",
        "max_depth":        4,
        "n_estimators":     200,
        "learning_rate":    0.05,
        "subsample":        0.8,
        "colsample_bytree": 0.8,
        "min_child_weight": 5,
        "tree_method":      "hist",
        "seed":             42,
    }

    # Train
    evals = [(dtrain, "train"), (dval, "val")]
    # XGBoost 2.0.x style training with early stopping
    model = xgb.train(params, dtrain, num_boost_round=params["n_estimators"], evals=evals, early_stopping_rounds=20, verbose_eval=False)

    # Evaluate
    train_preds = model.predict(dtrain)
    val_preds = model.predict(dval)

    train_auc = roc_auc_score(y_train, train_preds)
    val_auc = roc_auc_score(y_val, val_preds)

    val_acc = accuracy_score(y_val, (val_preds > 0.5).astype(int))

    print(f"Train AUC: {train_auc:.4f}")
    print(f"Validation AUC: {val_auc:.4f}")
    print(f"Validation Accuracy: {val_acc:.4f}")

    # Top features
    importance = model.get_score(importance_type="weight")
    top_features = sorted(importance.items(), key=lambda x: x[1], reverse=True)[:5]
    print("Top 5 features by weight:")
    for feat, weight in top_features:
        print(f"  {feat}: {weight}")

    model.save_model("quant_lab/model.json")
    print("Model saved to quant_lab/model.json")

if __name__ == "__main__":
    main()
