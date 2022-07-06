#pragma once

#include <Common/SettingsChanges.h>
#include <Parsers/IAST.h>


namespace DB
{

/** SET query
  */
class ASTSetQuery : public IAST
{
public:
    bool is_standalone = true; /// If false, this AST is a part of another query, such as SELECT.
    bool is_clone = false; /// If true, this AST is a clone from other part of the query and should not be printed in format()

    SettingsChanges changes;

    /** Get the text that identifies this element. */
    String getID(char) const override { return "Set"; }

    ASTPtr clone() const override { return std::make_shared<ASTSetQuery>(*this); }

    void formatImpl(const FormatSettings & format, FormatState &, FormatStateStacked) const override;

    void updateTreeHashImpl(SipHash & hash_state) const override;
};

}
