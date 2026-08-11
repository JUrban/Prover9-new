function clear_profiles(    key) {
  for (key in profile)
    delete profile[key]
  profile_count = 0
}

function clean_value(value) {
  sub(/[.]$/, "", value)
  return value
}

function remember_profile(line,    fields, count, i, token, at, key, value) {
  sub(/^Compact_query_profile: /, "", line)
  profile_count++
  count = split(line, fields, /, /)
  for (i = 1; i <= count; i++) {
    token = fields[i]
    at = index(token, "=")
    if (at != 0) {
      key = substr(token, 1, at - 1)
      value = clean_value(substr(token, at + 1))
      profile[profile_count SUBSEP key] = value
    }
  }
}

function value(row, key) {
  return profile[row SUBSEP key]
}

function json_escape(text,    result, i, ch) {
  result = ""
  for (i = 1; i <= length(text); i++) {
    ch = substr(text, i, 1)
    if (ch == "\\" || ch == "\"")
      result = result "\\" ch
    else if (ch == "\n")
      result = result "\\n"
    else if (ch == "\r")
      result = result "\\r"
    else if (ch == "\t")
      result = result "\\t"
    else
      result = result ch
  }
  return result
}

function json_number(text) {
  return text == "" ? "null" : text
}

function emit_file(    row, prefix) {
  if (current_file == "")
    return
  for (row = 1; row <= profile_count; row++) {
    if (output_format == "json") {
      if (json_rows != 0)
        print ","
      printf "  {\"file\":\"%s\",\"component\":\"%s\",\"op\":\"%s\"", \
        json_escape(current_file), json_escape(value(row, "component")), \
        json_escape(value(row, "op"))
      printf ",\"seconds\":%s,\"queries\":%s,\"candidates\":%s,\"work\":%s", \
        json_number(value(row, "seconds")), json_number(value(row, "queries")), \
        json_number(value(row, "candidates")), json_number(value(row, "work"))
      printf ",\"live\":%s,\"dead\":%s,\"duplicates\":%s,\"successes\":%s", \
        json_number(value(row, "live")), json_number(value(row, "dead")), \
        json_number(value(row, "duplicates")), json_number(value(row, "successes"))
      printf ",\"exact_tests\":%s,\"exact_successes\":%s,\"materialized\":%s", \
        json_number(value(row, "exact_tests")), \
        json_number(value(row, "exact_successes")), \
        json_number(value(row, "materialized"))
      printf ",\"bytes_decoded\":%s,\"candidate_p50_upper\":%s", \
        json_number(value(row, "bytes_decoded")), \
        json_number(value(row, "candidate_p50_upper"))
      printf ",\"candidate_p95_upper\":%s,\"candidate_p99_upper\":%s,\"candidate_max\":%s", \
        json_number(value(row, "candidate_p95_upper")), \
        json_number(value(row, "candidate_p99_upper")), \
        json_number(value(row, "candidate_max"))
      printf ",\"work_p50_upper\":%s,\"work_p95_upper\":%s,\"work_p99_upper\":%s,\"work_max\":%s", \
        json_number(value(row, "work_p50_upper")), \
        json_number(value(row, "work_p95_upper")), \
        json_number(value(row, "work_p99_upper")), \
        json_number(value(row, "work_max"))
      printf ",\"exact_p50_upper\":%s,\"exact_p95_upper\":%s,\"exact_p99_upper\":%s,\"exact_max\":%s", \
        json_number(value(row, "exact_p50_upper")), \
        json_number(value(row, "exact_p95_upper")), \
        json_number(value(row, "exact_p99_upper")), \
        json_number(value(row, "exact_max"))
      printf ",\"buckets\":\"%s\",\"given\":%s,\"generated\":%s,\"kept\":%s", \
        json_escape(value(row, "buckets")), json_number(given), \
        json_number(generated), json_number(kept)
      printf ",\"user_cpu\":%s,\"wall\":%s,\"peak_rss_kb\":%s}", \
        json_number(user_cpu), json_number(wall), json_number(peak_rss)
      json_rows++
    }
    else {
      printf "%s\t%s\t%s", current_file, value(row, "component"), \
        value(row, "op")
      printf "\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s", \
        value(row, "seconds"), value(row, "queries"), \
        value(row, "candidates"), value(row, "work"), value(row, "live"), \
        value(row, "dead"), value(row, "duplicates"), value(row, "successes")
      printf "\t%s\t%s\t%s\t%s", value(row, "exact_tests"), \
        value(row, "exact_successes"), value(row, "materialized"), \
        value(row, "bytes_decoded")
      printf "\t%s\t%s\t%s\t%s", value(row, "candidate_p50_upper"), \
        value(row, "candidate_p95_upper"), value(row, "candidate_p99_upper"), \
        value(row, "candidate_max")
      printf "\t%s\t%s\t%s\t%s", value(row, "work_p50_upper"), \
        value(row, "work_p95_upper"), value(row, "work_p99_upper"), \
        value(row, "work_max")
      printf "\t%s\t%s\t%s\t%s", value(row, "exact_p50_upper"), \
        value(row, "exact_p95_upper"), value(row, "exact_p99_upper"), \
        value(row, "exact_max")
      printf "\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", value(row, "buckets"), \
        given, generated, kept, user_cpu, wall, peak_rss
    }
  }
}

BEGIN {
  if (output_format == "json")
    print "["
  else
    print "file\tcomponent\top\tseconds\tqueries\tcandidates\twork\tlive\tdead\tduplicates\tsuccesses\texact_tests\texact_successes\tmaterialized\tbytes_decoded\tcandidate_p50_upper\tcandidate_p95_upper\tcandidate_p99_upper\tcandidate_max\twork_p50_upper\twork_p95_upper\twork_p99_upper\twork_max\texact_p50_upper\texact_p95_upper\texact_p99_upper\texact_max\tbuckets\tgiven\tgenerated\tkept\tuser_cpu\twall\tpeak_rss_kb"
}

/^@@P9_FILE / {
  emit_file()
  clear_profiles()
  current_file = substr($0, 11)
  given = generated = kept = user_cpu = wall = peak_rss = ""
  next
}

/^Compact_query_profile: / {
  remember_profile($0)
  next
}

/^Given=/ {
  if (match($0, /Given=[0-9]+/))
    given = substr($0, RSTART + 6, RLENGTH - 6)
  if (match($0, /Generated=[0-9]+/))
    generated = substr($0, RSTART + 10, RLENGTH - 10)
  if (match($0, /Kept=[0-9]+/))
    kept = substr($0, RSTART + 5, RLENGTH - 5)
  next
}

/^User_CPU=/ {
  if (match($0, /User_CPU=[0-9.]+/))
    user_cpu = substr($0, RSTART + 9, RLENGTH - 9)
  if (match($0, /Wall_clock=[0-9.]+/))
    wall = substr($0, RSTART + 11, RLENGTH - 11)
  sub(/[.]$/, "", wall)
  next
}

/^Allocator_slabs:/ {
  if (match($0, /RSS_kb: current=[0-9]+, peak=[0-9]+/)) {
    peak_text = substr($0, RSTART, RLENGTH)
    sub(/^.*peak=/, "", peak_text)
    peak_rss = peak_text
  }
  next
}

END {
  emit_file()
  if (output_format == "json")
    print "\n]"
}
