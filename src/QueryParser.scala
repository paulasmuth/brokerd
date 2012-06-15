package com.paulasmuth.fyrehose

class QueryParser {

  var query  : Query = null
  val lexer  = new QueryLexer(this)

  def parse(bdy: QueryBody) = {

    for (pos <- new Range(0, bdy.raw.size - 1, 1))
      lexer.next(bdy.raw(pos).toChar)

    if (query == null)
      throw new ParseException("query must contain one of stream, info, etc.")

    query
  }


  def emit(token: FQL_TOKEN) : Unit = {
    println("emit: " + token.getClass.getName)
    token match {

    case t: FQL_STREAM  =>
      if (query == null)
        query = build_query(t)
      else
        throw new ParseException("query can only contain one of stream, info, etc.")

    case t: FQL_TOKEN =>
      if (query == null)
        throw new ParseException("invalid query: must start with stream, info, etc.")
      else
        query.eval(t)

  } }


  def build_query(token: FQL_TOKEN) : Query = token match {

    case t: FQL_STREAM =>
      new StreamQuery()

  }

}
