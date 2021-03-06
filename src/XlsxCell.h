#ifndef READXL_XLSXCELL_
#define READXL_XLSXCELL_

#include <Rcpp.h>
#include "rapidxml.h"
#include "CellType.h"

// Key reference for understanding the structure of the XML is
// ECMA-376 (http://www.ecma-international.org/publications/standards/Ecma-376.htm)
// Section and page numbers below refer to the 4th edition
// 18.3.1.4   c           (Cell)       [p1598]
// 18.3.1.96  v           (Cell Value) [p1709]
// 18.18.11   ST_CellType (Cell Type)  [p2443]

// Simple parser: does not check that order of numbers and letters is correct
inline std::pair<int, int> parseRef(const char* ref) {
  int col = 0, row = 0;

  for (const char* cur = ref; *cur != '\0'; ++cur) {
    if (*cur >= '0' && *cur <= '9') {
      row = row * 10 + (*cur - '0');
    } else if (*cur >= 'A' && *cur <= 'Z') {
      col = 26 * col + (*cur - 'A' + 1);
    } else {
      Rcpp::stop("Invalid character '%s' in cell ref '%s'", *cur, ref);
    }
  }

  return std::make_pair(row - 1, col - 1); // zero indexed
}

// Parser for <si> and <is> inlineStr tags CT_Rst [p3893]
// returns true if a string is found, false if missing.
inline bool parseString(const rapidxml::xml_node<>* string, std::string *out) {
  bool found = false;
  out->clear();

  const rapidxml::xml_node<>* t = string->first_node("t");
  if (t != NULL) {
    // According to the spec (CT_Rst, p3893) a single <t> element
    // may coexist with zero or more <r> elements.
    //
    // However, software that read these files do not appear to exclusively
    // follow this spec.
    //
    // MacOSX preview, considers only the <t> element if found and ignores any
    // additional r elements.
    //
    //
    // Excel 2010 appears to produce only elements with r elements or with a
    // single t element and no mixtures. It will, however, consider an element
    // with a single t element followed by one or more r elements as valid,
    // concatenating the results. Any other combination of r and t elements
    // is considered invalid.
    //
    // We read the <t> tag, if present, first, then concatenate any <r> tags.
    // All Excel 2010 sheets will read correctly under this regime.
    *out = std::string(t->value());
    found = true;
  }
  // iterate over all r elements
  for (const rapidxml::xml_node<>* r = string->first_node("r"); r != NULL;
       r = r->next_sibling("r")) {
    // a unique t element should be present (CT_RElt [p3893])
    // but MacOSX preview just ignores chunks with no t element present
    const rapidxml::xml_node<>* t = r->first_node("t");
    if (t != NULL) {
      *out += t->value();
      found = true;
    }
  }
  return found;
}

class XlsxCell {
  rapidxml::xml_node<>* cell_;
  std::pair<int,int> location_;

public:

  XlsxCell(rapidxml::xml_node<>* cell): cell_(cell) {
    rapidxml::xml_attribute<>* ref = cell_->first_attribute("r");
    if (ref == NULL)
      Rcpp::stop("Invalid cell: lacks ref attribute");

    location_ = parseRef(ref->value());
  }

  int row() {
    return location_.first;
  }

  int col() {
    return location_.second;
  }

  std::string asStdString(const std::vector<std::string>& stringTable) {
    rapidxml::xml_node<>* v = cell_->first_node("v");
    if (v == NULL)
      return "[NULL]";

    rapidxml::xml_attribute<>* t = cell_->first_attribute("t");
    if (t == NULL || strncmp(t->value(), "s", 3) != 0)
      return std::string(v->value());

    int id = atoi(v->value());
    return stringTable.at(id);
  }

  double asDouble(const std::string& na) {
    rapidxml::xml_node<>* v = cell_->first_node("v");
    if (v == NULL || na.compare(v->value()) == 0)
      return NA_REAL;

    return (v == NULL) ? 0 : atof(v->value());
  }

  double asDate(const std::string& na, int offset) {
    rapidxml::xml_node<>* v = cell_->first_node("v");
    if (v == NULL || na.compare(v->value()) == 0)
      return NA_REAL;

    double value = atof(v->value());
    return (v == NULL) ? 0 : (value - offset) * 86400;
  }

  Rcpp::RObject asCharSxp(const std::string& na,
                          const std::vector<std::string>& stringTable) {

    // Is it an inline string?  // 18.3.1.53 is (Rich Text Inline) [p1649]
    rapidxml::xml_node<>* is = cell_->first_node("is");
    if (is != NULL) {
      std::string value;
      if (!parseString(is, &value) || na.compare(value) == 0) {
        return NA_STRING;
      } else {
        return Rf_mkCharCE(value.c_str(), CE_UTF8);
      }
    }


    rapidxml::xml_node<>* v = cell_->first_node("v");
    if (v == NULL)
      return NA_STRING;

    rapidxml::xml_attribute<>* t = cell_->first_attribute("t");

    if (t != NULL && strncmp(t->value(), "s", t->value_size()) == 0) {
      return stringFromTable(v->value(), na, stringTable);
    } else {
      if (na.compare(v->value()) == 0) {
        return NA_STRING;
      } else {
        return Rf_mkCharCE(v->value(), CE_UTF8);
      }
    }
  }

  CellType type(const std::string& na,
                const std::vector<std::string>& stringTable,
                const std::set<int>& dateStyles) {
    rapidxml::xml_attribute<>* t = cell_->first_attribute("t");

    if (t == NULL || strncmp(t->value(), "n", 5) == 0) {
      rapidxml::xml_attribute<>* s = cell_->first_attribute("s");
      int style = (s == NULL) ? -1 : atoi(s->value());

      return (dateStyles.count(style) > 0) ? CELL_DATE : CELL_NUMERIC;
    } else if (strncmp(t->value(), "b", 5) == 0) {
      // TODO
      return CELL_NUMERIC;
    } else if (strncmp(t->value(), "d", 5) == 0) {
      // Does excel use this? Regardless, don't have cross-platform ISO8601
      // parser (yet) so need to return as text
      return CELL_TEXT;
    } else if (strncmp(t->value(), "e", 5) == 0) { // error
      return CELL_BLANK;
    } else if (strncmp(t->value(), "s", 5) == 0) { // string in string table
      rapidxml::xml_node<>* v = cell_->first_node("v");
      if (v == NULL)
        return CELL_BLANK;

      int id = atoi(v->value());
      const std::string& string = stringTable.at(id);
      return (string == na) ? CELL_BLANK : CELL_TEXT;
    } else if (strncmp(t->value(), "str", 5) == 0) { // formula
      rapidxml::xml_node<>* v = cell_->first_node("v");
      if (v == NULL)
        return CELL_BLANK;

      return (na.compare(v->value()) == 0) ? CELL_BLANK : CELL_TEXT;
    } else if  (strncmp(t->value(), "inlineStr", 9) == 0) { // formula
      return CELL_TEXT;
    } else {
      Rcpp::warning("[%i, %i]: unknown type '%s'",
        row() + 1, col() + 1, t->value());
      return CELL_TEXT;
    }

    return CELL_NUMERIC;
  }

private:


  Rcpp::RObject stringFromTable(const char* val, const std::string& na,
                                const std::vector<std::string>& stringTable) {
    int id = atoi(val);
    if (id < 0 || id >= (int) stringTable.size()) {
      Rcpp::warning("[%i, %i]: Invalid string id %i", row() + 1, col() + 1, id);
      return NA_STRING;
    }

    const std::string& string = stringTable.at(id);
    return (string == na) ? NA_STRING : Rf_mkCharCE(string.c_str(), CE_UTF8);
  }

};


#endif
