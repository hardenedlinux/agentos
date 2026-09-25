# Content Marketing Pipeline

Standard pipeline shape for cross-border e-commerce content marketing goals.
Use this as the default decomposition; adjust step count and order to fit
the specific goal — not every goal needs all three stages.

## Stage 1 — Keyword Extraction

Extract and prioritize SEO keywords relevant to the product or category
named in the goal. Input is typically raw product information (title,
category, attributes) supplied by the merchant. Output is a ranked keyword
list, tagged by search intent (informational / transactional / navigational)
where determinable.

Suggested capability: `seo.extract_keywords`

## Stage 2 — Listing Generation

Generate the product listing copy itself (title, bullet points, long
description) using the product information and the keyword list from Stage
1 as input. Output must read naturally to a human buyer first; keyword
placement is secondary to readability and conversion intent.

Suggested capability: `content.generate_listing`

## Stage 3 — SEO Optimization

Review the generated listing copy against the keyword list and platform
SEO conventions (title length limits, keyword density, required
disclosure fields). Output is either an optimized revision of the listing
or a structured list of specific suggested edits.

Suggested capability: `seo.optimize`

## Notes for a Fresh Deployment

If none of the three suggested capabilities above are yet registered in
this Registry, set `needs_forge: true` on the corresponding step and use
the suggested capability name as the `command` value — this triggers Forge
to generate the Worker on first use. Subsequent jobs in this domain will
find the capability already registered and can set `needs_forge: false`.

## Multilingual Variants

If the goal asks for content in more than one language, add a step per
target language after Stage 2 rather than folding translation into the
listing-generation step itself — this keeps each step's `input`/`output`
shape uniform and lets a single `translate.batch`-style capability (if
registered) serve any domain, not just content marketing.
