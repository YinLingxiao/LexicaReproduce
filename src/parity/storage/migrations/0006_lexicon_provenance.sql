-- 0006_lexicon_provenance · 词频与等级来源
--
-- frequency_source:
--   ecdict.frq / ecdict.bnc  = 新导入时实际采用的 ECDICT 字段
--   seed_approx              = 种子词库的近似排序值，不是语料库排名
--   NULL                     = 来源未记录
-- level_source:
--   seed                     = 种子词库人工编撰标签
--   legacy_unverified        = 旧版导入器自动推断、无法追溯来源的 A1-C2
--   NULL                     = 未记录或没有等级
--
-- 旧数据保留原值，只补充“来源未记录/旧版推断”标记，不覆盖个人数据。

ALTER TABLE words ADD COLUMN frequency_source TEXT;
ALTER TABLE senses ADD COLUMN level_source TEXT;

UPDATE words
SET frequency_source = CASE
  WHEN source = 'seed' THEN 'seed_approx'
  ELSE NULL
END
WHERE frequency_source IS NULL AND frequency_rank IS NOT NULL;

UPDATE senses
SET level_source = CASE
  WHEN (SELECT source FROM words WHERE words.id = senses.word_id) = 'seed' THEN 'seed'
  ELSE 'legacy_unverified'
END
WHERE level_source IS NULL
  AND level IS NOT NULL
  AND TRIM(level) <> '';
