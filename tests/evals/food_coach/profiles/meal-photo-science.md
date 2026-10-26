---
title: Meal photo science
desc: read BEFORE estimating calories or macros from a meal photo or a food description, or before changing the food log
inject: spawn
---
# Meal-photo nutrition estimation

## Purpose
Estimate food mass, calories, protein, carbohydrate, fat and fibre from one or more meal photos. Treat image-only estimates as uncertain ranges, not measurements. Use known labels, recipes, package data and weighed portions whenever available.

## Required inputs
Ask for, if available:
- The original photo, ideally overhead plus a side view.
- A scale reference: known plate or bowl size, cutlery, a package.
- Food identity, recipe or brand, cooking method, sauces and oils, and anything hidden underneath.
- For validation only: weighed grams and nutrition labels. Keep these blinded until the estimate is locked.

If critical context is absent, proceed but widen the uncertainty and name the missing information that matters most.

## Procedure
1. Inventory before numbers: list every visible component separately, including drinks, sauces, dressings, oil and toppings.
2. Estimate the mass of each component as a range, then convert with per-100 g values.
3. Report a range per item and a total range. Say which values are estimated, not from a label.
4. Never present a point value as certain.

## Logging
The food log is one file: food-log.csv in project nutrition. Read its tail before estimating anything logged before, and compute the day's total from the file's own rows. Every figure the owner states, and every correction, appends a row with files.append.
