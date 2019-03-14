--------------------------------------------------------------------------------
-- Copyright 2001-2012, Daan Leijen, Bastiaan Heeren, Jurriaan Hage. This file 
-- is distributed under the terms of the BSD3 License. For more information, 
-- see the file "LICENSE.txt", which is included in the distribution.
--------------------------------------------------------------------------------
--  $Id$

module Lvm.Core.Expr 
   ( CoreModule, CoreDecl, Expr(..), Binds(..), Bind(..)
   , Alts, Alt(..), Pat(..), Literal(..), Con(..), Variable(..)
   ) where

import Prelude hiding ((<$>))
import Lvm.Common.Byte
import Lvm.Common.Id
import Lvm.Core.Module
import Lvm.Core.PrettyId
import Lvm.Core.Type
import Text.PrettyPrint.Leijen

----------------------------------------------------------------
-- Modules
----------------------------------------------------------------
type CoreModule = Module Expr
type CoreDecl   = Decl Expr

----------------------------------------------------------------
-- Core expressions:
----------------------------------------------------------------
data Expr       = Let       !Binds Expr       
                | Match     !Id Alts
                | Ap        Expr Expr
                | ApType    !Expr !Type
                | Lam       !Variable Expr
                | Forall    !Quantor !Kind !Expr
                | Con       !(Con Expr)
                | Var       !Id
                | Lit       !Literal

data Variable = Variable { variableName :: !Id, variableType :: !Type }
data Binds      = Rec       ![Bind]
                | Strict    !Bind
                | NonRec    !Bind

data Bind       = Bind      !Variable !Expr

type Alts       = [Alt]
data Alt        = Alt       !Pat Expr

data Pat        = PatCon    !(Con Tag) ![Id]
                | PatLit    !Literal
                | PatDefault

data Literal    = LitInt    !Int
                | LitDouble !Double
                | LitBytes  !Bytes

data Con tag    = ConId  !Id
                | ConTag tag !Arity

----------------------------------------------------------------
-- Pretty printing
----------------------------------------------------------------

instance Pretty Expr where
  pretty = ppExpr 0 []

ppExpr :: Int -> QuantorNames -> Expr -> Doc
ppExpr p quantorNames expr
  = case expr of
    Match x as  -> prec 0 $ align (text "match" <+> ppVarId x <+> text "with" <+> text "{" <$> indent 2 (ppAlts quantorNames as)
                              <+> text "}")
    Let bs x    -> prec 0 $ align (ppLetBinds quantorNames bs (text "in" <+> ppExpr 0 quantorNames x))
    Lam (Variable x t) e -> prec 0 $ text "\\" <> ppVarId x <> text ": " <> pretty t <+> text "->" <+> ppExpr 0 quantorNames e
    Forall quantor k e ->
      let
        quantorNames' = case quantor of
          Quantor idx (Just name) -> (idx, name) : quantorNames
          _ -> quantorNames
      in
        prec 0 $ text "forall" <+> text (show quantor) <> text ": " <> pretty k <> text "." <+> ppExpr 0 quantorNames' e
    Ap e1 e2    -> prec 9 $ ppExpr 9 quantorNames e1 <+> ppExpr 10 quantorNames e2
    ApType e1 t -> prec 9 $ ppExpr 9 quantorNames e1 <+> text "{ " <> pretty t <> text " }"
    Var x       -> ppVarId x
    Con con     -> pretty con
    Lit lit     -> pretty lit
  where
    prec p'  | p' >= p   = id
             | otherwise = parens

instance Pretty a => Pretty (Con a) where
   pretty con =
      case con of
         ConId x          -> ppConId x
         ConTag tag arity -> parens (char '@' <> pretty tag <> comma <> pretty arity)
 
----------------------------------------------------------------
--
----------------------------------------------------------------

ppLetBinds :: QuantorNames -> Binds -> Doc -> Doc
ppLetBinds quantorNames binds doc
  = case binds of
      NonRec bind -> nest 4 (text "let" <+> ppBind quantorNames bind) <$> doc
      Strict bind -> nest 5 (text "let!" <+> ppBind quantorNames bind) <$> doc
      Rec recs    -> nest 4 (text "let" <+> ppBindList quantorNames recs) <$> doc -- let rec not parsable

ppBind :: QuantorNames -> Bind -> Doc
ppBind quantorNames (Bind (Variable x t) expr) =
  nest 2 (ppId  x <> text ": " <+> pretty t <> text " = " <+> ppExpr 0 quantorNames expr <> semi)

ppBindList :: QuantorNames -> [Bind] -> Doc
ppBindList quantorNames = vcat . map (ppBind quantorNames)

ppAlt :: QuantorNames -> Alt -> Doc
ppAlt quantorNames (Alt pat expr) =
      nest 4 (pretty pat <+> text "->" </> ppExpr 0 quantorNames expr <> semi)

ppAlts :: QuantorNames -> [Alt] -> Doc
ppAlts quantorNames = vcat . map (ppAlt quantorNames)

----------------------------------------------------------------
--
----------------------------------------------------------------

instance Pretty Pat where 
   pretty pat = 
      case pat of
         PatCon con ids -> hsep (pretty con : map ppVarId ids)
         PatLit lit  -> pretty lit
         PatDefault  -> text "_"

instance Pretty Literal where 
   pretty lit = 
      case lit of
         LitInt i    -> pretty i
         LitDouble d -> pretty d
         LitBytes s  -> text (show (stringFromBytes s))
