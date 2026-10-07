package com.rawr.camera.settings.preferences

/** Converts supported 3D .cube files to the renderer's numeric and domain syntax. */
internal object CubeNormalizer {
    const val MIN_SIZE = 2
    const val MAX_SIZE = 65
    private val whitespace = Regex("\\s+")
    private val decimal = Regex("[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?")

    /** Rejects unsupported directives instead of silently changing the LUT's meaning. */
    fun normalize(lines: Sequence<String>): List<String> {
        val rows = ArrayList<String>()
        var size: Int? = null
        var domainMin: List<Float>? = null
        var domainMax: List<Float>? = null
        var inputRange: List<Float>? = null
        fun numbers(tokens: List<String>, count: Int, label: String): List<Float> {
            require(tokens.size == count) { "$label needs $count numbers" }
            return tokens.map { token ->
                val value = if (decimal.matches(token)) token.toFloatOrNull() else null
                require(value != null && value.isFinite()) { "$label needs finite decimal numbers" }
                value
            }
        }
        for (raw in lines) {
            val line = raw.substringBefore('#').trim()
            if (line.isEmpty()) continue
            val tokens = line.split(whitespace)
            val head = tokens.first()
            when (head) {
                "TITLE" -> Unit
                "LUT_1D_SIZE" -> throw IllegalArgumentException("1D LUTs are not supported")
                "LUT_3D_SIZE" -> {
                    require(size == null) { "Duplicate LUT_3D_SIZE" }
                    val n = tokens.getOrNull(1)?.toIntOrNull()
                    require(tokens.size == 2 && n != null && n in MIN_SIZE..MAX_SIZE) {
                        "LUT size must be between $MIN_SIZE and $MAX_SIZE points per axis"
                    }
                    size = n
                }
                "DOMAIN_MIN" -> {
                    require(domainMin == null) { "Duplicate DOMAIN_MIN" }
                    domainMin = numbers(tokens.drop(1), 3, head)
                }
                "DOMAIN_MAX" -> {
                    require(domainMax == null) { "Duplicate DOMAIN_MAX" }
                    domainMax = numbers(tokens.drop(1), 3, head)
                }
                "LUT_3D_INPUT_RANGE" -> {
                    require(inputRange == null) { "Duplicate LUT_3D_INPUT_RANGE" }
                    inputRange = numbers(tokens.drop(1), 2, head)
                }
                else -> {
                    require(head.first() in '0'..'9' || head.first() in "+-.") {
                        "Unsupported .cube directive: $head"
                    }
                    val n = requireNotNull(size) { "LUT rows come before LUT_3D_SIZE" }
                    require(rows.size < n * n * n) { "Too many LUT rows for size $n" }
                    rows.add(numbers(tokens, 3, "A LUT row").joinToString(" "))
                }
            }
        }
        val n = requireNotNull(size) { "Missing LUT_3D_SIZE" }
        require(rows.size == n * n * n) { "LUT table has ${rows.size} rows, expected ${n * n * n} for size $n" }
        val range = inputRange
        if (range != null) {
            require(range[1] > range[0]) { "LUT_3D_INPUT_RANGE maximum must exceed minimum" }
            require(domainMin?.all { it == range[0] } != false && domainMax?.all { it == range[1] } != false) {
                "LUT_3D_INPUT_RANGE conflicts with DOMAIN_MIN or DOMAIN_MAX"
            }
        }
        val min = domainMin ?: List(3) { range?.get(0) ?: 0f }
        val max = domainMax ?: List(3) { range?.get(1) ?: 1f }
        require((0..2).all { max[it] > min[it] }) { "DOMAIN_MAX must exceed DOMAIN_MIN on every channel" }
        return buildList {
            add("LUT_3D_SIZE $n")
            add("DOMAIN_MIN ${min.joinToString(" ")}")
            add("DOMAIN_MAX ${max.joinToString(" ")}")
            addAll(rows)
        }
    }
}
