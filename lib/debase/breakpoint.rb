module Debase
  class Breakpoint

    attr_reader :expr, :id, :pos, :source

    def initialize(id, file, line, expr)
      @id = id
      @pos = line
      @source = file
      @expr = expr
    end
  end
end