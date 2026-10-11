#pragma once
// A previous assistant reply carrying a ~1.2 KB code block, shaped like the one
// bench unit B produced on 2026-10-10 (CUM-485): the "add a plot" follow-up
// needs the exact names it defines (stock_by_core, stock_0_100cm_Mg_C_ha), and
// the old 300 B row clip cut the reply before either was defined. Synthetic
// (not a byte-frozen recording), so tests may reuse it freely.
namespace harness_test {
inline const char* kPrevReplyCodeSnippet = R"SNIP(Assuming `depth_cm` is the top of each sampled interval, this computes the stock per core:

```python
import pandas as pd

TOP_DEPTH_CM = 100

df = pd.read_csv("sediment_cores.csv")
required = {"core_id", "depth_cm", "dry_bulk_density_g_cm3", "corg_percent"}
missing = required - set(df.columns)
if missing:
    raise ValueError(f"missing columns: {sorted(missing)}")

df = df.sort_values(["core_id", "depth_cm"]).reset_index(drop=True)
df["next_depth_cm"] = df.groupby("core_id")["depth_cm"].shift(-1)
df["thickness_cm"] = (df["next_depth_cm"] - df["depth_cm"]).fillna(1.0)
df = df[df["depth_cm"] < TOP_DEPTH_CM].copy()
df["thickness_cm"] = df[["thickness_cm"]].clip(upper=TOP_DEPTH_CM - df["depth_cm"], axis=0)

# Mg C per hectare: g/cm3 * (%/100) * cm * 100 (unit conversion to Mg/ha)
df["stock_Mg_C_ha"] = (
    df["dry_bulk_density_g_cm3"] * (df["corg_percent"] / 100.0) * df["thickness_cm"] * 100.0
)

stock_by_core = (
    df.groupby("core_id", as_index=False)["stock_Mg_C_ha"]
    .sum()
    .rename(columns={"stock_Mg_C_ha": "stock_0_100cm_Mg_C_ha"})
)
print(stock_by_core)
print(f"{len(stock_by_core)} cores, mean stock "
      f"{stock_by_core['stock_0_100cm_Mg_C_ha'].mean():.1f} Mg C/ha")
```

The result frame is `stock_by_core`, one row per core.)SNIP";
}  // namespace harness_test
